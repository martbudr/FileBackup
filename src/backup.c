#define _GNU_SOURCE

#include "backup.h"

#include "copy.h"
#include "utils.h"
#include "watcher_thread.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>

Backup* find_backup(BackupManager* mgr, const char* src)
{
    // szuka backupu folderu src
    Backup* b = mgr->backups;
    while (b)
    {
        if (strcmp(b->source, src) == 0)
            return b;
        b = b->next;
    }
    return NULL;
}

int backup_to_target(BackupManager* mgr, Backup* b, char* target)
{
    if (!mgr || !b || !b->source || !target)
        return -1;

    if (strlen(b->source) >= PATH_MAX || strlen(target) >= PATH_MAX)
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    int nested = is_same_or_subdirectory(b->source, target);
    if (nested < 0)
    {
        perror("Cannot validate backup paths");
        return -1;
    }
    if (nested)
    {
        fprintf(stderr, "Destination must not be the source directory or inside it: %s\n", target);
        return -1;
    }

    if (ensure_empty_dir_or_create(target) == -1)
    {
        return -1;
    }

    if (prepare_backup_target(b, target) == -1)
        return -1;

    copy_recursive(b->source, target, "");

    return start_backup_watcher(mgr, b, target);
}

void start_backup(BackupManager* mgr, char* src, char** targets, int n)
{
    pthread_mutex_lock(&mgr->state_lock);
    Backup* b = find_backup(mgr, src);

    if (b)
    {  // jesli src juz byl gdziekolwiek kopiowany i ma aktywny backup
        // sprawdzamy czy ktorykolwiek z target folderow nie jest juz przypadkiem w backupie
        for (int i = 0; i < n; ++i)
        {
            if (are_same_dir(src, targets[i]))
            {
                fprintf(stderr, "Failed to create backup.\n");
                fprintf(stderr, "Destination path must be different from source path: %s\n", targets[i]);
                continue;
            }

            // sprawdzamy czy nie ma backupu z src do dst juz utworzonego
            bool backup_exists = false;
            for (int j = 0; j < b->target_count; ++j)
            {
                if (strcmp(targets[i], b->targets[j]) == 0)
                {
                    backup_exists = true;
                    break;
                }
            }
            if (backup_exists) {
                fprintf(stderr, "Backup: %s -> %s already exists.\n", src, targets[i]);
            }
            else
            {  // jesli nie ma to tworzymy
                if (backup_to_target(mgr, b, targets[i]) == -1)
                {
                    fprintf(stderr, "Failed to create backup to target: %s.\n", targets[i]);
                    continue;
                }
            }
        }
    }
    else
    {  // jesli src jeszcze nie bylo backupowane
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

        for (int i = 0; i < n; ++i)
        {
            if (are_same_dir(src, targets[i]))
            {
                fprintf(stderr, "Failed to create backup.\n");
                fprintf(stderr, "Destination path must be different from source path: %s\n", targets[i]);
                continue;
            }

            if (backup_to_target(mgr, b, targets[i]) == -1)
            {
                fprintf(stderr, "Failed to create backup to target: %s.\n", targets[i]);
                continue;
            }
        }

        // wstawianie backupu na poczatek listy:
        if (b->target_count > 0)
        {
            b->next = mgr->backups;
            mgr->backups = b;
        }
        else
        {
            free(b->targets);
            free(b->child_tids);
            free(b->source);
            free(b);
        }
    }

    pthread_mutex_unlock(&mgr->state_lock);
    return;
}

void stop_backup_to_targets(BackupManager* mgr, char* src, char** targets, int n)
{
    // targets - ones to which we stop the backup

    pthread_mutex_lock(&mgr->state_lock);
    Backup* b = find_backup(mgr, src);
    if (!b)
    {  // target does not exist in backup
        fprintf(stderr, "Failed to stop backup: Backup of %s doesn't exists.\n", src);
        pthread_mutex_unlock(&mgr->state_lock);
        return;
    }

    for (int i = 0; i < n; ++i)
    {  // dla kazdego z targetow do ktorych chcemy zatrzymac
        if (are_same_dir(src, targets[i]))
        {
            fprintf(stderr, "Failed to stop backup %s -> %s.\n", src, targets[i]);
            fprintf(stderr, "Destination path must be different from source path: %s\n", src);
            continue;
        }

        bool stopped = false;
        for (int j = 0; j < b->target_count; ++j)
        {
            if (strcmp(b->targets[j], targets[i]) == 0)
            {
                pthread_cancel(b->child_tids[j]);  // zeby watek sie zakonczyl

                // Release the state lock while waiting, then reacquire it before updating records.
                pthread_mutex_unlock(&mgr->state_lock);
                pthread_join(b->child_tids[j], NULL);
                pthread_mutex_lock(&mgr->state_lock);

                // przesuniecie tablicy targets do przodu
                free(b->targets[j]);
                for (int k = j; k < b->target_count - 1; ++k)
                {
                    b->targets[k] = b->targets[k + 1];
                    b->child_tids[k] = b->child_tids[k + 1];
                }
                b->target_count--;
                printf("Stopped backup %s -> %s.\n", src, targets[i]);
                stopped = true;
                break;
            }
            if (stopped)
                break;
        }
        if (!stopped)
            fprintf(stderr, "Failed to stop backup %s -> %s.\n", src, targets[i]);
    }

    pthread_mutex_unlock(&mgr->state_lock);
    return;
}

void cleanup_all_backups(BackupManager* mgr)
{
    pthread_mutex_lock(&mgr->state_lock);
    // zabijanie dzieci
    Backup* b = mgr->backups;
    while (b)
    {
        Backup* next = b->next;
        for (int i = 0; i < b->target_count; i++)
        {
            if (b->child_tids[i] > 0)
            {
                pthread_cancel(b->child_tids[i]);
                pthread_join(b->child_tids[i], NULL);
            }
        }

        // zwalnianie pamieci
        free(b->source);
        if (b->targets)
        {
            for (int i = 0; i < b->target_count; ++i)
            {
                free(b->targets[i]);
            }
            free(b->targets);
        }

        free(b->child_tids);
        free(b);

        b = next;
    }

    // wyczyszczenie struktury mgr
    mgr->backups = NULL;
    pthread_mutex_unlock(&mgr->state_lock);
    pthread_mutex_destroy(&mgr->state_lock);
}

void list_backups(BackupManager* mgr)
{
    pthread_mutex_lock(&mgr->state_lock);
    Backup* b = mgr->backups;
    while (b)
    {
        printf("Backup: %s ->", b->source);
        for (int i = 0; i < b->target_count; i++)
            printf(" %s", b->targets[i]);
        printf("\n");
        b = b->next;
    }
    pthread_mutex_unlock(&mgr->state_lock);
}
