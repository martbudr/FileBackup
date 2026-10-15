#define _GNU_SOURCE

#include "copy.h"

#include "utils.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void copy_file(char* src, char* dst)
{
    struct stat st;
    if (lstat(src, &st) == -1)
        ERR("lstat");

    int fd_in, fd_out;
    if ((fd_in = TEMP_FAILURE_RETRY(open(src, O_RDONLY))) < 0)
        ERR("open");
    if ((fd_out = TEMP_FAILURE_RETRY(open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777))) < 0)
        ERR("open");

    int FILE_BUF_LEN = 8192;
    char buf[FILE_BUF_LEN];
    for (;;)
    {
        const ssize_t read_size = bulk_read(fd_in, buf, FILE_BUF_LEN);
        if (read_size == -1)
            ERR("bulk_read");
        if (read_size == 0)
            break;
        if (bulk_write(fd_out, buf, read_size) == -1)
            ERR("bulk_write");
    }
    if (TEMP_FAILURE_RETRY(close(fd_out)) == -1)
        ERR("close");

    if (TEMP_FAILURE_RETRY(close(fd_in)) == -1)
        ERR("close");
}

void copy_symlink(char* src_root, char* dst_root, char* rel_path)
{
    char src[PATH_MAX], dst[PATH_MAX];
    snprintf(src, PATH_MAX, "%s/%s", src_root, rel_path);
    snprintf(dst, PATH_MAX, "%s/%s", dst_root, rel_path);

    char link_target[PATH_MAX];
    // czyta symlink:
    ssize_t len = readlink(src, link_target, sizeof(link_target) - 1);
    if (len == -1)
    {
        fprintf(stderr, "Failed to read symlink.\n");
        return;
    }
    link_target[len] = '\0';

    // symlink - bezwzgledny w srodku source_root
    if (link_target[0] == '/' && strncmp(link_target, src_root, strlen(src_root)) == 0)
    {
        // budowanie nowego symlinka
        const char* relative = link_target + strlen(src_root);
        char new_target[PATH_MAX];
        snprintf(new_target, sizeof(new_target), "%s%s", dst_root, relative);

        // ustawianie nowego symlinka jako docelowy
        strncpy(link_target, new_target, sizeof(link_target) - 1);
        link_target[sizeof(link_target) - 1] = '\0';
    }

    // tworzenie nowego symlinka (przekopiowany z src)
    if (symlink(link_target, dst) == -1)
    {
        fprintf(stderr, "Failed to copy symlink %s -> %s as %s -> %s.\n", src, link_target, dst, link_target);
    }
}

void copy_recursive(char* src_root, char* dst_root, char* rel_path)
{  // kopiowanie calej struktury
    // Obsługuje zarówno zwykłe pliki, jak i dowiązania symboliczne
    // Symlink w katalogu zrodlowym zawiera sceizke bezwzgledna prowadzaca do tego katalogu =>
    //   nowy symlink wskazuje na odpowiedni plik w katalogu docelowym
    // wpp przekopiowanie sciezki symlinku

    char src[PATH_MAX], dst[PATH_MAX];
    if (strlen(rel_path) == 0)
    {
        snprintf(src, PATH_MAX, "%s", src_root);
        snprintf(dst, PATH_MAX, "%s", dst_root);
    }
    else
    {
        snprintf(src, PATH_MAX, "%s/%s", src_root, rel_path);
        snprintf(dst, PATH_MAX, "%s/%s", dst_root, rel_path);
    }

    struct stat st;
    if (lstat(src, &st) != 0)
        ERR("lstat");

    if (S_ISDIR(st.st_mode))
    {
        if (mkdir(dst, st.st_mode) == -1 && errno != EEXIST)
            ERR("mkdir");         // tworzenie dst dir
        DIR* dir = opendir(src);  // wywolania recursive dla tego co w dir
        if (!dir)
            ERR("opendir");

        struct dirent* dp;
        while ((dp = readdir(dir)) != NULL)
        {
            if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0)
                continue;

            // wywolanie rekursji
            char* rel_path2 = malloc(PATH_MAX);
            if (strlen(rel_path) == 0)
                snprintf(rel_path2, PATH_MAX, "%s", dp->d_name);
            else
                snprintf(rel_path2, PATH_MAX, "%s/%s", rel_path, dp->d_name);

            copy_recursive(src_root, dst_root, rel_path2);
            free(rel_path2);
        }
        closedir(dir);
    }
    else if (S_ISLNK(st.st_mode))
    {
        copy_symlink(src_root, dst_root, rel_path);
    }
    else if (S_ISREG(st.st_mode))
    {
        // kopiowanie pliku

        copy_file(src, dst);
    }
    else
    {
        ERR("st_mode");
    }
}
