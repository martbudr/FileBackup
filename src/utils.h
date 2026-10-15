#ifndef UTILS_H
#define UTILS_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#define ERR(source) \
    (fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), perror(source), kill(0, SIGKILL), exit(EXIT_FAILURE))

ssize_t bulk_read(int fd, char* buf, size_t count);

ssize_t bulk_write(int fd, char* buf, size_t count);

int ensure_empty_dir_or_create(char* path);

bool are_same_dir(char* path1, char* path2);

// Returns 1 for an equal/nested target, 0 for an outside target, or -1 on error (errno set).
int is_same_or_subdirectory(const char* source, const char* target);

void free_args(char** args, char* line, int argc);

#endif
