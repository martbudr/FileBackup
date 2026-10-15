#ifndef RESTORE_H
#define RESTORE_H

#include "backup.h"

int copy_file_if_needed(char* src, char* dst);

int restore_recursive(char* src_root, char* dst_root, char* rel_path);

int delete_missing(char* src_root, char* dst_root, char* rel_path);

int restore_target(BackupManager* mgr, Backup* b, char* target);

void restore_backup(BackupManager* mgr, char* src, char* target);

#endif
