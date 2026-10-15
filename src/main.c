#define _GNU_SOURCE

#include "main.h"

#include "backup.h"
#include "parser.h"
#include "restore.h"
#include "utils.h"

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

volatile sig_atomic_t stop_flag = 0;

void* signal_thread(void* arg)
{
    sigset_t waitset;
    sigemptyset(&waitset);
    sigaddset(&waitset, SIGINT);
    sigaddset(&waitset, SIGTERM);

    int sig;
    sigwait(&waitset, &sig);
    printf("Received signal %d, exiting...\n", sig);

    stop_flag = 1;

    return NULL;
}

void usage(char* cmd)
{
    if (strcmp(cmd, "add") == 0)
    {
        fprintf(stderr, "USAGE: %s <source path> <target paths...>\n", cmd);
    }
    else if (strcmp(cmd, "end") == 0)
    {
        fprintf(stderr, "USAGE: %s <source path> <target paths...>\n", cmd);
    }
    else if (strcmp(cmd, "restore") == 0)
    {
        fprintf(stderr, "USAGE: %s <source path> <target path>\n", cmd);
    }
    else if (strcmp(cmd, "list") == 0)
    {
        fprintf(stderr, "USAGE: %s\n", cmd);
    }
    else if (strcmp(cmd, "exit") == 0)
    {
        fprintf(stderr, "USAGE: %s\n", cmd);
    }
}

int main(int argc, char** argv)
{
    // testing parsing file names

    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);  // blokujemy wszystkie sygnaly w glownym watku

    BackupManager mgr = {.backups = NULL};

    pthread_mutex_init(&mgr.state_lock, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, signal_thread, NULL);
    pthread_detach(tid);

    printf("List of the available commands:\n");
    printf("add <source> <targets...> - create new backup from source to targets\n");
    printf("end <source> <targets...> - stop backups from source to targets\n");
    printf("restore <source> <target> - restore backup from source to target\n");
    printf("list - list all backups\n");
    printf("exit - stop all backups and exit the program\n\n");

    while (!stop_flag)
    {
        printf("Enter a command: ");

        char* line = NULL;
        size_t len = 0;
        ssize_t read = getline(&line, &len, stdin);
        printf("\n");
        if (read < 0)
            ERR("getline");

        if (stop_flag)
        {
            free(line);
            break;
        }

        char** args;
        int arg_cnt = parse_args(line, &args);

        if (arg_cnt < 1)
        {
            fprintf(stderr, "Specify the command.\n");
            free(line);
            continue;
        }

        char* cmd = args[0];
        // ADD
        if (strcmp(cmd, "add") == 0)
        {
            if (arg_cnt < 3)
            {
                usage("add");
                free_args(args, line, arg_cnt);
                continue;
            }
            char* src = args[1];

            // sprawdzenie czy src katalog istnieje
            struct stat st;
            if (lstat(src, &st) == -1)
            {
                fprintf(stderr, "Source directory does not exist.\n");
                free_args(args, line, arg_cnt);
                continue;
            }

            // startowanie backupu
            int n = arg_cnt - 2;
            start_backup(&mgr, src, &args[2], n);
        }
        // END
        else if (strcmp(cmd, "end") == 0)
        {
            if (arg_cnt < 3)
            {
                usage("end");
                free_args(args, line, arg_cnt);
                continue;
            }
            char* src = args[1];

            int n = arg_cnt - 2;
            stop_backup_to_targets(&mgr, src, &args[2], n);
        }
        // RESTORE
        else if (strcmp(cmd, "restore") == 0)
        {
            if (arg_cnt != 3)
            {
                usage("restore");
                free_args(args, line, arg_cnt);
                continue;
            }
            char* src = args[1];
            char* dst = args[2];

            if (are_same_dir(src, dst))
            {
                fprintf(stderr, "Failed to restore backup.\n");
                fprintf(stderr, "Destination path must be different from source path: %s\n", src);
                free_args(args, line, arg_cnt);
                continue;
            }

            restore_backup(&mgr, src, dst);
        }
        // LIST
        else if (strcmp(cmd, "list") == 0)
        {
            if (arg_cnt > 2)
            {
                usage("list");
                free_args(args, line, arg_cnt);
                continue;
            }

            list_backups(&mgr);
        }
        // EXIT
        else if (strcmp(cmd, "exit") == 0)
        {
            if (arg_cnt > 2)
            {
                usage("exit");
                free_args(args, line, arg_cnt);
                continue;
            }

            free_args(args, line, arg_cnt);

            break;
        }
        else
        {
            fprintf(stderr, "Unknown command %s\n", cmd);
        }

        free_args(args, line, arg_cnt);
    }

    cleanup_all_backups(&mgr);
    return EXIT_SUCCESS;
}
