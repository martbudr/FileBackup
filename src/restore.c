#define _GNU_SOURCE

#include "restore.h"

#include "backup.h"
#include "copy.h"
#include "utils.h"
#include "watcher_thread.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void copy_file_if_needed(char* src, char* dst)
{
    struct stat sb_src, sb_dst;
    if (lstat(src, &sb_src) < 0)
        ERR("lstat");  // nie mozna otworzyc pliku wejsciowego

    if (lstat(dst, &sb_dst) == 0)
    {  // plik w dst juz istnieje
        // jest potrzeba tylko jesli plik byl modyfikowany
        bool content_changed = (sb_src.st_mtime != sb_dst.st_mtime);
        bool perms_changed = ((sb_src.st_mode & 0777) != (sb_dst.st_mode & 0777));

        if (content_changed)
            copy_file(src, dst);

        if (perms_changed)
            chmod(dst, sb_src.st_mode & 0777);

        return;
    }

    // plik w dst nie istnieje - tworzymy
    copy_file(src, dst);
}

int restore_recursive(char* src_root, char* dst_root, char* rel_path)
{
    // przywraca kopie zapasowa - kopiuje tylko kiedy files/dirs sa rozne
    char src[PATH_MAX], dst[PATH_MAX];
    snprintf(src, sizeof(src), "%s/%s", src_root, rel_path);
    snprintf(dst, sizeof(dst), "%s/%s", dst_root, rel_path);

    struct stat sb_src, sb_dst;
    if (lstat(src, &sb_src) < 0)
        ERR("lstat");

    if (S_ISREG(sb_src.st_mode))
    {
        copy_file_if_needed(src, dst);
    }
    else if (S_ISDIR(sb_src.st_mode))
    {
        if (lstat(dst, &sb_dst) < 0)
        {
            if (mkdir(dst, sb_src.st_mode & 0777) < 0 && errno != EEXIST)
                return -1;
        }

        if ((sb_src.st_mtime != sb_dst.st_mtime) &&
            ((sb_src.st_mode & 0777) != (sb_dst.st_mode & 0777)))  // jesli takie same to konczymy
            return 0;

        DIR* d = opendir(src);
        if (!d)
        {
            fprintf(stderr, "Failed to open directory %s.\n", src);
            return -1;
        }

        struct dirent* dp;
        while ((dp = readdir(d)) != NULL)
        {
            if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, ".."))
                continue;

            char new_rel[PATH_MAX];
            snprintf(new_rel, sizeof(new_rel), "%s/%s", rel_path[0] ? rel_path : "", dp->d_name);

            if (restore_recursive(src_root, dst_root, new_rel) < 0)
            {
                closedir(d);
                return -1;
            }
        }
        closedir(d);
    }
    else if (S_ISLNK(sb_src.st_mode))
    {
        copy_symlink(src_root, dst_root, rel_path);
    }
    return 0;
}

void delete_missing(char* src_root, char* dst_root, char* rel_path)
{
    char src[PATH_MAX], dst[PATH_MAX];
    snprintf(src, sizeof(src), "%s/%s", src_root, rel_path);
    snprintf(dst, sizeof(dst), "%s/%s", dst_root, rel_path);

    struct stat sb_src, sb_dst;
    if (lstat(dst, &sb_dst) < 0)
        ERR("lstat");

    bool dst_nonexist = (lstat(src, &sb_src) < 0);  // czy plik/katalog nie istnieje w src
    if (dst_nonexist && !S_ISDIR(sb_dst.st_mode))
    {  // niekatalog ktory nie istnieje w src
        unlink(dst);
        return;
    }

    if (dst_nonexist || S_ISDIR(sb_dst.st_mode))
    {  // plik/katalog ktory nie istnieje w src lub katalog (wchodzimy rekursywnie)
        DIR* d = opendir(dst);
        if (!dst_nonexist && !d)
        {
            fprintf(stderr, "Could not open existing directory (%s).\n", dst);
            return;
        }

        struct dirent* dp;
        while ((dp = readdir(d)) != NULL)
        {
            if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, ".."))
                continue;

            char rel_path2[PATH_MAX];
            snprintf(rel_path2, sizeof(rel_path2), "%s/%s", rel_path[0] ? rel_path : "", dp->d_name);
            delete_missing(src_root, dst_root, rel_path2);
        }

        closedir(d);
        if (dst_nonexist)
        {
            if (rmdir(dst) != 0)
                ERR("rmdir");
        }
    }
    return;
}

int restore_target(BackupManager* mgr, Backup* b, char* target)
{
    if (!mgr || !b || !target)
        return -1;

    // realokacja targets
    char** new_targets = realloc(b->targets, (b->target_count + 1) * sizeof(char*));
    if (!new_targets)
        return -1;
    b->targets = new_targets;

    // realokacja child_tids
    pthread_t* new_tids = realloc(b->child_tids, (b->target_count + 1) * sizeof(pthread_t));
    if (!new_tids)
        return -1;
    b->child_tids = new_tids;

    b->targets[b->target_count] = strdup(target);
    if (!b->targets[b->target_count])
        return -1;

    // kopiujemy calosc
    restore_recursive(b->source, target, "");
    delete_missing(b->source, target, "");

    // tworzymy watcher w osobnym wątku
    pthread_t tid;
    struct
    {
        char source[PATH_MAX];
        char target[PATH_MAX];
        BackupManager* mgr;
    }* watcher_args = malloc(sizeof(*watcher_args));

    strcpy(watcher_args->source, b->source);
    strcpy(watcher_args->target, target);
    watcher_args->mgr = mgr;

    if (pthread_create(&tid, NULL, watcher_thread, watcher_args) != 0)
    {
        free(watcher_args);
        return -1;
    }

    b->child_tids[b->target_count] = tid;
    b->target_count++;
    return 0;
}

void restore_backup(BackupManager* mgr, char* src, char* target)
{
    pthread_mutex_lock(&mgr->state_lock);
    Backup* b = find_backup(mgr, src);

    if (are_same_dir(src, target))
    {
        fprintf(stderr, "Failed to create backup.\n");
        fprintf(stderr, "Destination path must be different from source path: %s\n", target);
        pthread_mutex_unlock(&mgr->state_lock);
        return;
    }

    bool newly_created = false;

    if (b)
    {  // jesli jest jakis backup src w cos
        // sprawdzamy czy backup nie istnieje juz
        bool backup_exists = false;
        for (int j = 0; j < b->target_count; ++j)
        {
            if (strcmp(target, b->targets[j]) == 0)
            {
                backup_exists = true;
                break;
            }
        }
        if (backup_exists)
        {
            fprintf(stderr, "Backup: %s -> %s already exists.\n", src, target);
            pthread_mutex_unlock(&mgr->state_lock);
            return;
        }
    }
    else
    {
        // tworzymy nowy backup
        b = malloc(sizeof(Backup));
        b->source = strdup(src);
        b->targets = NULL;
        b->child_tids = NULL;
        b->target_count = 0;
        newly_created = true;
    }

    if (restore_target(mgr, b, target) == -1)
    {
        free(b->source);
        free(b);
        fprintf(stderr, "Failed to create backup to target: %s.\n", target);
        pthread_mutex_unlock(&mgr->state_lock);
        return;
    }

    if (newly_created)
    {  // tylko wtedy kiedy nowy, bo inaczej juz jest na tej liscie
       // dopiero tutaj bo nie chcemy zeby tworzylo w przypadku kiedy restore_target failuje
        b->next = mgr->backups;
        mgr->backups = b;
    }

    pthread_mutex_unlock(&mgr->state_lock);
    return;
}
