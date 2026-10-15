#ifndef WATCHERTHREAD_H
#define WATCHERTHREAD_H

#include <limits.h>
#include <stdint.h>

#define MAX_MOVED_EVENTS 100
#define MAX_WATCHES 1024

typedef struct MoveEventTracker
{
    uint32_t cookie;
    char name[NAME_MAX];
} MoveEventTracker;

typedef struct WatchDescriptorMap
{
    int wd;
    char path[PATH_MAX];
} WatchDescriptorMap;

struct ThreadResources
{
    void* data;
    MoveEventTracker* moved_events;
    WatchDescriptorMap* wd_map;
};

int delete_content(char* path);
int remove_tracked_move(uint32_t cookie, char* original_name_out, MoveEventTracker* moved_events,
                        int* move_event_count);
void add_tracked_move(uint32_t cookie, const char* name, MoveEventTracker* moved_events, int* move_event_count);
void cleanup_watcher_thread(void* arg);
int copy_file_or_dir(char* source, char* target);
void* watcher_thread(void* arg);

#endif
