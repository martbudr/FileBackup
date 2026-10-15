#define _GNU_SOURCE

#include "utils.h"

#include "watcher_thread.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

ssize_t bulk_read(int fd, char* buf, size_t count)
{
    ssize_t c;
    ssize_t len = 0;
    do
    {
        c = TEMP_FAILURE_RETRY(read(fd, buf, count));
        if (c < 0)
            return c;
        if (c == 0)
            return len;  // EOF
        buf += c;
        len += c;
        count -= c;
    } while (count > 0);
    return len;
}

ssize_t bulk_write(int fd, char* buf, size_t count)
{
    ssize_t c;
    ssize_t len = 0;
    do
    {
        c = TEMP_FAILURE_RETRY(write(fd, buf, count));
        if (c < 0)
            return c;
        buf += c;
        len += c;
        count -= c;
    } while (count > 0);
    return len;
}

int ensure_empty_dir_or_create(char* path)
{
    // zwraca -1 gdy katalog istnieje i nie jest pusty
    // w przeciwnym przypadku zwraca 0

    struct stat st;
    bool isempty = true;
    // dir istnieje
    if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode))
    {
        DIR* dir;
        if ((dir = opendir(path)) == NULL)
            ERR("opendir");

        // sprawdzenie czy jest pusty
        struct dirent* dp;
        while ((dp = readdir(dir)) != NULL)
        {
            if (strcmp(dp->d_name, ".") != 0 && strcmp(dp->d_name, "..") != 0)
            {
                isempty = false;
                break;
            }
        }
        if (!isempty)
        {
            fprintf(stderr, "Directory not empty: %s\n", path);
            closedir(dir);
            return -1;
        }

        closedir(dir);
    }
    else
    {  // tworzymy dir bo nie istnieje
        if (mkdir(path, 0777) == 0)
            printf("Created directory: %s\n", path);
        else
            ERR("mkdir");
    }

    return 0;
}

bool are_same_dir(char* path1, char* path2)
{
    // sprawdza czy path1 wskazuje na ten sam katalog co path2

    char* realpath1 = realpath(path1, NULL);
    char* realpath2 = realpath(path2, NULL);

    if (!realpath1 || !realpath2)
    {
        free(realpath1);
        free(realpath2);
        return false;
    }

    bool same = strcmp(realpath1, realpath2) == 0;

    free(realpath1);
    free(realpath2);

    return same;
}

// Resolve existing components through realpath, including symlinks. Missing
// components are resolved against their parent without creating anything.
static char* resolve_target_path(const char* path)
{
    char* resolved = realpath(path, NULL);
    if (resolved || errno != ENOENT)
        return resolved;

    struct stat st;
    if (lstat(path, &st) == 0)
    {
        errno = ENOENT;  // Do not treat a dangling symlink as a missing directory
        return NULL;
    }
    if (errno != ENOENT)
        return NULL;

    char* copy = strdup(path);
    if (!copy)
        return NULL;
    size_t len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/')
        copy[--len] = '\0';

    char* slash = strrchr(copy, '/');
    const char* name = slash ? slash + 1 : copy;
    const char* parent = ".";
    if (slash)
    {
        *slash = '\0';
        parent = slash == copy ? "/" : copy;
    }
    char* base = resolve_target_path(parent);
    if (!base)
    {
        int saved_errno = errno;
        free(copy);
        errno = saved_errno;
        return NULL;
    }

    if (strcmp(name, "..") == 0)
    {
        char* last = strrchr(base, '/');
        if (last == base)
            base[1] = '\0';
        else
            *last = '\0';
        resolved = base;
    }
    else if (strcmp(name, ".") == 0)
        resolved = base;
    else
    {
        resolved = malloc(strlen(base) + strlen(name) + 2);
        if (resolved)
            sprintf(resolved, "%s%s%s", base, strcmp(base, "/") == 0 ? "" : "/", name);
        free(base);
    }
    free(copy);
    return resolved;
}

int is_same_or_subdirectory(const char* source, const char* target)
{
    if (!source || !target || !*source || !*target)
    {
        errno = EINVAL;
        return -1;
    }
    char* source_path = realpath(source, NULL);
    if (!source_path)
        return -1;
    char* target_path = resolve_target_path(target);
    if (!target_path)
    {
        int saved_errno = errno;
        free(source_path);
        errno = saved_errno;
        return -1;
    }

    size_t len = strlen(source_path);
    int result = strcmp(source_path, "/") == 0 ||
                 (strncmp(source_path, target_path, len) == 0 &&
                  (target_path[len] == '\0' || target_path[len] == '/'));
    free(source_path);
    free(target_path);
    return result;
}

void free_args(char** args, char* line, int argc)
{
    if (args)
    {
        for (int i = 0; i < argc; i++)
            free(args[i]);
        free(args);
    }
    free(line);
}

int prepare_backup_target(Backup* b, char* target)
{
    if (!b || !b->source || !target)
        return -1;

    if (strlen(b->source) >= PATH_MAX || strlen(target) >= PATH_MAX)
    {
        errno = ENAMETOOLONG;
        return -1;
    }

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

    return 0;
}

int start_backup_watcher(BackupManager* mgr, Backup* b, const char* target)
{
    // tworzymy watcher w osobnym wątku
    pthread_t tid;
    WatcherArgs* watcher_args = malloc(sizeof(*watcher_args));
    if (!watcher_args)
        goto fail;

    strcpy(watcher_args->source, b->source);
    strcpy(watcher_args->target, target);
    watcher_args->mgr = mgr;

    if (pthread_create(&tid, NULL, watcher_thread, watcher_args) != 0)
    {
        free(watcher_args);
        goto fail;
    }

    b->child_tids[b->target_count] = tid;
    b->target_count++;

    return 0;

fail:
    free(b->targets[b->target_count]);
    b->targets[b->target_count] = NULL;
    return -1;
}
