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

// Metadata alone cannot detect edits that preserve size and modification time.
static int files_differ(const char* src, const char* dst)
{
    FILE* in = fopen(src, "rb");
    if (!in)
        return -1;
    FILE* out = fopen(dst, "rb");
    if (!out)
    {
        fclose(in);
        return -1;
    }
    char left[8192], right[8192];
    int result = 0;
    for (;;)
    {
        size_t a = fread(left, 1, sizeof(left), in);
        size_t b = fread(right, 1, sizeof(right), out);
        if (ferror(in) || ferror(out))
        {
            result = -1;
            break;
        }
        if (a != b || memcmp(left, right, a) != 0)
        {
            result = 1;
            break;
        }
        if (a == 0)
            break;
    }
    fclose(in);
    fclose(out);
    return result;
}

int copy_file_if_needed(char* src, char* dst)
{
    struct stat sb_src, sb_dst;
    if (lstat(src, &sb_src) < 0)
        return -1;

    if (lstat(dst, &sb_dst) == 0)
    {  // plik w dst juz istnieje
        // jest potrzeba tylko jesli plik byl modyfikowany
        if (!S_ISREG(sb_dst.st_mode))
        {
            errno = EINVAL;
            return -1;
        }
        int content_changed = files_differ(src, dst);
        if (content_changed < 0)
            return -1;
        bool perms_changed = ((sb_src.st_mode & 0777) != (sb_dst.st_mode & 0777));

        if (content_changed)
            copy_file(src, dst);

        if (perms_changed)
            return chmod(dst, sb_src.st_mode & 0777);

        return 0;
    }
    if (errno != ENOENT)
        return -1;

    // plik w dst nie istnieje - tworzymy
    copy_file(src, dst);
    return 0;
}

int restore_recursive(char* src_root, char* dst_root, char* rel_path)
{
    // przywraca kopie zapasowa - kopiuje tylko kiedy files/dirs sa rozne
    char src[PATH_MAX], dst[PATH_MAX];
    if (snprintf(src, sizeof(src), "%s/%s", src_root, rel_path) >= (int)sizeof(src) ||
        snprintf(dst, sizeof(dst), "%s/%s", dst_root, rel_path) >= (int)sizeof(dst))
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    struct stat sb_src, sb_dst;
    if (lstat(src, &sb_src) < 0)
        return -1;

    if (S_ISREG(sb_src.st_mode))
    {
        return copy_file_if_needed(src, dst);
    }
    else if (S_ISDIR(sb_src.st_mode))
    {
        if (lstat(dst, &sb_dst) < 0)
        {
            if (errno != ENOENT || mkdir(dst, (sb_src.st_mode & 0777) | 0700) < 0)
                return -1;
        }
        else if (!S_ISDIR(sb_dst.st_mode))
        {
            errno = ENOTDIR;
            return -1;
        }

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
            if (snprintf(new_rel, sizeof(new_rel), "%s/%s", rel_path, dp->d_name) >= (int)sizeof(new_rel))
            {
                closedir(d);
                errno = ENAMETOOLONG;
                return -1;
            }

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
        if (lstat(dst, &sb_dst) == 0)
        {
            if (S_ISDIR(sb_dst.st_mode) || unlink(dst) < 0)
                return -1;
        }
        else if (errno != ENOENT)
            return -1;
        copy_symlink(src_root, dst_root, rel_path);
        if (lstat(dst, &sb_dst) < 0)
            return -1;
    }
    return 0;
}

int delete_missing(char* src_root, char* dst_root, char* rel_path)
{
    char src[PATH_MAX], dst[PATH_MAX];
    if (snprintf(src, sizeof(src), "%s/%s", src_root, rel_path) >= (int)sizeof(src) ||
        snprintf(dst, sizeof(dst), "%s/%s", dst_root, rel_path) >= (int)sizeof(dst))
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    struct stat sb_src, sb_dst;
    if (lstat(dst, &sb_dst) < 0)
        return -1;

    bool dst_nonexist = (lstat(src, &sb_src) < 0);
    if (dst_nonexist && errno != ENOENT)
        return -1;
    // Czy plik/katalog nie istnieje w src.
    if (dst_nonexist && !S_ISDIR(sb_dst.st_mode))
    {  // niekatalog ktory nie istnieje w src
        return unlink(dst);
    }

    if (dst_nonexist || S_ISDIR(sb_dst.st_mode))
    {  // plik/katalog ktory nie istnieje w src lub katalog (wchodzimy rekursywnie)
        DIR* d = opendir(dst);
        if (!d)
        {
            fprintf(stderr, "Could not open existing directory (%s).\n", dst);
            return -1;
        }

        struct dirent* dp;
        while ((dp = readdir(d)) != NULL)
        {
            if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, ".."))
                continue;

            char rel_path2[PATH_MAX];
            if (snprintf(rel_path2, sizeof(rel_path2), "%s/%s", rel_path, dp->d_name) >= (int)sizeof(rel_path2))
            {
                closedir(d);
                errno = ENAMETOOLONG;
                return -1;
            }
            if (delete_missing(src_root, dst_root, rel_path2) < 0)
            {
                closedir(d);
                return -1;
            }
        }

        closedir(d);
        if (dst_nonexist)
        {
            if (rmdir(dst) != 0)
                return -1;
        }
        else if (chmod(dst, sb_src.st_mode & 0777) < 0)
            return -1;
    }
    return 0;
}

int restore_target(BackupManager* mgr, Backup* b, char* target)
{
    if (!mgr || !b || !b->source || !target)
        return -1;

    // Restore may update a populated destination, but neither tree may contain the other.
    int nested = is_same_or_subdirectory(b->source, target);
    if (nested != 0)
        return -1;
    struct stat st;
    if (lstat(target, &st) == 0)
    {
        if (!S_ISDIR(st.st_mode) || is_same_or_subdirectory(target, b->source) != 0)
            return -1;
    }
    else if (errno != ENOENT)
        return -1;

    if (prepare_backup_target(b, target) == -1)
        return -1;

    if (restore_recursive(b->source, target, "") < 0 ||
        delete_missing(b->source, target, "") < 0)
    {
        free(b->targets[b->target_count]);
        b->targets[b->target_count] = NULL;
        return -1;
    }

    return start_backup_watcher(mgr, b, target);
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
        if (!b)
        {
            pthread_mutex_unlock(&mgr->state_lock);
            return;
        }
        b->source = strdup(src);
        if (!b->source)
        {
            free(b);
            pthread_mutex_unlock(&mgr->state_lock);
            return;
        }
        b->targets = NULL;
        b->child_tids = NULL;
        b->target_count = 0;
        newly_created = true;
    }

    if (restore_target(mgr, b, target) == -1)
    {
        if (newly_created)
        {
            free(b->targets);
            free(b->child_tids);
            free(b->source);
            free(b);
        }
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
