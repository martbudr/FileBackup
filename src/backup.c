#define _GNU_SOURCE

#include "backup.h"

#include "copy.h"
#include "utils.h"
#include "watcher_thread.h"

#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if (!mgr || !b || !target)
        return -1;

    if (ensure_empty_dir_or_create(target) == -1)
    {
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

    // kopiujemy calosc
    copy_recursive(b->source, target, "");

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
            if (backup_exists)
            {
                fprintf(stderr, "Backup: %s -> %s already exists.\n", src, targets[i]);
                continue;
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
        b->source = strdup(src);
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

                // join
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
                pthread_mutex_unlock(&mgr->state_lock);
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
