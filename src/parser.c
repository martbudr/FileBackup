#define _GNU_SOURCE

#include "parser.h"

#include "utils.h"

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>

int parse_args(const char* line, char*** out)
{
    // parsuje podane przez uzytkownika komendy
    const char* p = line;  // aktualny znak
    int argc = 0;
    int cap = 8;
    char** args = malloc(cap * sizeof(char*));
    if (!args)
        ERR("malloc");

    while (*p)
    {
        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
        if (*p == '\0')
            break;

        char* buf = malloc(PATH_MAX);
        if (!buf)
            ERR("malloc");
        int len = 0;

        bool in_quotes = false;
        bool in_single_quotes = false;

        // argument
        while (*p)
        {
            if (!in_single_quotes && !in_quotes && (*p == ' ' || *p == '\t'))
                break;

            if (*p == '\'' && !in_quotes)
            {
                in_single_quotes = !in_single_quotes;
                p++;
                continue;
            }
            if (in_single_quotes)
            {
                buf[len++] = *p++;
                continue;
            }

            if (*p == '"')
            {  // poczatek quote
                in_quotes = !in_quotes;
                p++;
                continue;
            }

            if (*p == '\\')
            {  // escape
                p++;
                if (!*p)
                    break;
                if (*p == '"' || *p == '\\' || *p == '$' || *p == '`' ||
                    (!in_quotes && *p == ' '))  // tylko to escapujemy
                    buf[len++] = *p++;
                else
                    buf[len++] = '\\';
                continue;
            }

            buf[len++] = *p++;
        }

        // koniec argumentu - dodanie go do argumentow
        buf[len] = '\0';
        if (argc >= cap)
        {
            cap *= 2;
            char** tmp = realloc(args, cap * sizeof(*args));
            if (!tmp)
                ERR("realloc");
            args = tmp;
        }

        // usuwanie whitespace z konca
        while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\t' || buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';

        args[argc++] = buf;
    }

    *out = args;
    return argc;
}
