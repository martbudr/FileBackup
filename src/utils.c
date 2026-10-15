#define _GNU_SOURCE

#include "utils.h"

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
