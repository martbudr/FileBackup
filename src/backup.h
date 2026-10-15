#ifndef BACKUP_H
#define BACKUP_H

#include <pthread.h>

typedef struct Backup
{
    char* source;
    char** targets;
    int target_count;
    pthread_t* child_tids;  // procesy watcherow
    struct Backup* next;
} Backup;

typedef struct BackupManager
{
    Backup* backups;
    pthread_mutex_t state_lock;
} BackupManager;

Backup* find_backup(BackupManager* mgr, const char* src);

int backup_to_target(BackupManager* mgr, Backup* b, char* target);

void start_backup(BackupManager* mgr, char* src, char** targets, int n);

void stop_backup_to_targets(BackupManager* mgr, char* src, char** targets, int n);

void cleanup_all_backups(BackupManager* mgr);

void list_backups(BackupManager* mgr);

#endif
