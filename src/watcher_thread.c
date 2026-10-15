#define _GNU_SOURCE
#include "watcher_thread.h"

#include <dirent.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/inotify.h>
#include <sys/stat.h>

int delete_content(char* path)
{
    struct stat st;
    if (lstat(path, &st) < 0)
    {
        if (errno == ENOENT)
            return 0;  // juz nie ma
        perror("lstat");
        return -1;
    }

    if (S_ISDIR(st.st_mode))
    {
        DIR* d = opendir(path);
        if (!d)
        {
            perror("opendir");
            return -1;
        }

        struct dirent* p;
        while ((p = readdir(d)))
        {
            if (strcmp(p->d_name, ".") == 0 || strcmp(p->d_name, "..") == 0)
                continue;

            char full_path[PATH_MAX];
            snprintf(full_path, PATH_MAX, "%s/%s", path, p->d_name);

            if (delete_content(full_path) < 0)
            {
                fprintf(stderr, "Failed to delete from %s.\n", full_path);
                closedir(d);
                return -1;
            }
        }
        closedir(d);

        printf("Recursively deleting target directory: %s\n", path);
        return rmdir(path);  // usuwanie empty dir
    }
    else
    {
        // non-dir
        printf("Deleting target file: %s\n", path);
        return unlink(path);
    }
}

// funkcja do usuwania MOVED_FROM eventow
int remove_tracked_move(uint32_t cookie, char* original_name_out, MoveEventTracker* moved_events, int* move_event_count)
{
    for (int j = 0; j < *move_event_count; j++)
    {
        if (moved_events[j].cookie == cookie)
        {
            strncpy(original_name_out, moved_events[j].name, NAME_MAX);

            // przemieszczanie pozostalych elementow
            for (int k = j; k < *move_event_count - 1; k++)
            {
                moved_events[k] = moved_events[k + 1];
            }
            (*move_event_count)--;
            return 1;  // znaleziony i usuniety
        }
    }
    return 0;  // not found
}

// dodanie MOVED_FROM eventu do trackera
void add_tracked_move(uint32_t cookie, const char* name, MoveEventTracker* moved_events, int* move_event_count)
{
    if (*move_event_count < MAX_MOVED_EVENTS)
    {
        moved_events[*move_event_count].cookie = cookie;
        strncpy(moved_events[*move_event_count].name, name, NAME_MAX);
        (*move_event_count)++;
        printf("Started tracking move out: %s (Cookie: %u)\n", name, cookie);
    }
    else
    {
        fprintf(stderr, "Move event tracker is full.\n");
    }
}

void cleanup_watcher_thread(void* arg)
{
    struct ThreadResources* res = arg;
    if (res->moved_events)
    {
        free(res->moved_events);
        printf("Freed moved_events tracker.\n");
    }
    if (res->data)
    {
        free(res->data);
        printf("Freed thread data structure.\n");
    }
    if (res->wd_map)
    {
        free(res->wd_map);
        printf("Freed watch descriptor map.\n");
    }
    free(res);
    printf("Thread resources cleaned up.\n");
}

char* get_path_from_wd(int wd, struct WatchDescriptorMap* map, int count)
{
    for (int j = 0; j < count; j++)
    {
        if (map[j].wd == wd)
        {
            return map[j].path;
        }
    }
    return NULL;
}

int store_wd_map(int wd, const char* path, struct WatchDescriptorMap* map, int* count_ptr)
{
    int count = *count_ptr;
    if (count >= MAX_WATCHES)
    {
        fprintf(stderr, "Error: Max watches reached.\n");
        return -1;
    }
    map[count].wd = wd;
    strncpy(map[count].path, path, PATH_MAX);
    (*count_ptr)++;
    return 0;
}

void remove_wd_map(int wd, struct WatchDescriptorMap* map, int* count_ptr)
{
    int count = *count_ptr;
    for (int j = 0; j < count; j++)
    {
        if (map[j].wd == wd)
        {
            // przemieszczamy elementy zeby zapelnic dziure
            for (int k = j; k < count - 1; k++)
            {
                map[k] = map[k + 1];
            }
            (*count_ptr)--;
            break;
        }
    }
}

int calculate_target_path(const char* source_base_dir, const char* target_base_dir, const char* event_base_path,
                          const char* event_name, char* target_path_out, int target_path_size)
{
    // 1. znajdujemy poczatek relatywnej sciezki w event base path
    const char* relative_path_start = strstr(event_base_path, source_base_dir);

    if (relative_path_start == NULL || relative_path_start != event_base_path)
    {
        // baza sciezki zawsze powinna sie zaczynac source base dir
        fprintf(stderr, "Error: Base path calculation mismatch.\n");
        return -1;
    }

    // kalkulujemy offset za source_base_dir
    const char* relative_path_segment = event_base_path + strlen(source_base_dir);

    // 3. tworzymy koncowa target sciezke

    // zaczynamy z target base dir
    int len = snprintf(target_path_out, target_path_size, "%s", target_base_dir);

    if (len >= target_path_size || len < 0)
    {
        fprintf(stderr, "Error: Target base path too long.\n");
        return -1;
    }

    // dodawanie czesci relative path
    // pomijamy poczatkowe '/' jesli istnieje

    // dostosowujemy skipujac poczatkowe '/' jesli istnieje
    if (*relative_path_segment == '/')
    {
        relative_path_segment++;
    }

    // dolaczamy wyliczone rel path
    len += snprintf(target_path_out + len, target_path_size - len, "/%s", relative_path_segment);

    // dolaczamy event name (nazwe plik lub dir)
    len += snprintf(target_path_out + len, target_path_size - len, "/%s", event_name);

    if (len >= target_path_size || len < 0)
    {
        fprintf(stderr, "Error: Final target path too long.\n");
        return -1;
    }

    return 0;
}

int add_watches_recursively(int fd, const char* dir_path, const char* target_base, struct WatchDescriptorMap* map,
                            int* count_ptr)
{
    // dodanie watcha do curr dir
    int wd = inotify_add_watch(
        fd, dir_path, IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_ATTRIB);

    if (wd < 0)
    {
        perror("inotify_add_watch");
        return -1;
    }

    if (store_wd_map(wd, dir_path, map, count_ptr) < 0)
    {
        inotify_rm_watch(fd, wd);
        return -1;
    }

    printf("Added watch on: %s (WD: %d)\n", dir_path, wd);

    // rekurencja wszystkich subdir
    DIR* d = opendir(dir_path);
    if (!d)
        return 0;

    struct dirent* p;
    while ((p = readdir(d)))
    {
        if (strcmp(p->d_name, ".") == 0 || strcmp(p->d_name, "..") == 0)
            continue;

        char full_path[PATH_MAX];
        snprintf(full_path, PATH_MAX, "%s/%s", dir_path, p->d_name);

        struct stat st;
        if (lstat(full_path, &st) < 0)
            continue;

        if (S_ISDIR(st.st_mode))
        {
            add_watches_recursively(fd, full_path, target_base, map, count_ptr);
        }
    }
    closedir(d);
    return 0;
}

int copy_file_or_dir(char* source, char* target)
{
    struct stat st;
    if (lstat(source, &st) < 0)
    {
        perror("lstat source");
        return -1;
    }

    // jesli dir nie istnieje to tworzymy
    if (S_ISDIR(st.st_mode))
    {
        printf("Creating/Updating directory structure: %s\n", target);
        return mkdir(target, st.st_mode);
    }
    else
    {
        printf("Copying file: %s -> %s\n", source, target);
        copy_file(source, target);
        return 0;
    }
}

void* watcher_thread(void* arg)
{
    struct
    {
        char source[PATH_MAX];
        char target[PATH_MAX];
        BackupManager* mgr;
    }* data = arg;

    MoveEventTracker* moved_events = malloc(MAX_MOVED_EVENTS * sizeof *moved_events);
    if (!moved_events)
    {
        free(data);
        return NULL;
    }
    int move_event_count = 0;

    struct WatchDescriptorMap* wd_map = malloc(MAX_WATCHES * sizeof *wd_map);
    int wd_count = 0;

    struct ThreadResources* resources = malloc(sizeof(*resources));
    if (!resources)
    {
        free(moved_events);
        free(data);
        free(wd_map);
        return NULL;
    }
    resources->data = data;
    resources->moved_events = moved_events;
    resources->wd_map = wd_map;

    // dodawanie do cleanup routine
    pthread_cleanup_push(cleanup_watcher_thread, resources);

    // initowanie inotify
    int fd = inotify_init();
    if (fd < 0)
    {
        perror("inotify_init");
        free(data);
        free(moved_events);
        return NULL;
    }

    // dodanie watchy rekursywnie
    if (add_watches_recursively(fd, data->source, data->target, wd_map, &wd_count) < 0)
    {
        perror("inotify_add_watch");
        close(fd);
        free(data);
        free(moved_events);
        return NULL;
    }

    char buf[PATH_MAX];
    int running = 1;
    while (running)
    {
        ssize_t len = read(fd, buf, sizeof(buf));
        if (len < 0)
            break;

        ssize_t i = 0;
        while (i < len)
        {
            struct inotify_event* event = (struct inotify_event*)&buf[i];

            char* base_path = get_path_from_wd(event->wd, wd_map, wd_count);
            if (!base_path)
            {
                fprintf(stderr, "Error: Received event for unknown WD %d\n", event->wd);
                i += sizeof(struct inotify_event) + event->len;
                continue;
            }

            // jesli source lub target dir byla usunieta
            if (event->mask & IN_DELETE_SELF)
            {
                printf("Source directory deleted, deleting backup.\n");

                inotify_rm_watch(fd, event->wd);
                remove_wd_map(event->wd, wd_map, &wd_count);

                if (strcmp(base_path, data->source) == 0)
                    running = 0;

                i += sizeof(struct inotify_event) + event->len;
                continue;
            }

            if (event->len <= 0)
            {  // jesli bez sciezki
                i += sizeof(struct inotify_event) + event->len;
                continue;
            }

            // sciezki dla eventow
            char source_path[PATH_MAX];
            char target_path[PATH_MAX];
            snprintf(source_path, PATH_MAX, "%s/%s", base_path, event->name);

            if (calculate_target_path(data->source, data->target, base_path, event->name, target_path, PATH_MAX) != 0)
            {  // sciezka za dluga lub blednie liczona
                i += sizeof(struct inotify_event) + event->len;
                continue;
            }

            if (event->mask & (IN_CREATE | IN_MODIFY | IN_ATTRIB))
            {
                // IN_CREATE: New file or directory created
                // IN_MODIFY: File content changed
                // IN_ATTRIB: Metadata/Permissions changed (e.g., chmod)

                if (event->mask & IN_CREATE && event->mask & IN_ISDIR)
                {
                    printf("New directory created: %s. Adding watch.\n", source_path);

                    // dodaj watch dla nowego subdir
                    int new_wd = inotify_add_watch(
                        fd, source_path,
                        IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_ATTRIB);

                    if (new_wd >= 0)
                    {
                        store_wd_map(new_wd, source_path, wd_map, &wd_count);
                    }
                }

                printf("Create/Modify/Attrib: %s\n", event->name);
                copy_file_or_dir(source_path, target_path);
            }
            else if (event->mask & IN_DELETE)
            {
                printf("Delete: %s\n", event->name);
                delete_content(target_path);
            }
            else if (event->mask & IN_MOVED_FROM)
            {
                // plik/dir przemieszczony poza watched dir => moze byc rename lub przemieszczenie poza dir (z naszej
                // perspektywy usuniecie)
                printf("Moved From: %s (Cookie: %u)\n", event->name, event->cookie);
                add_tracked_move(event->cookie, event->name, moved_events,
                                 &move_event_count);  // czekamy na in_moved_to zeby zadecydowac co zrobimy
            }
            else if (event->mask & IN_MOVED_TO)
            {
                // przemieszczenie do watched dir lub druga czesc rename
                printf("Moved To: %s (Cookie: %u)\n", event->name, event->cookie);

                char original_name[NAME_MAX];

                if (event->cookie != 0 &&
                    remove_tracked_move(event->cookie, original_name, moved_events, &move_event_count))
                {
                    // rename

                    char old_target_path[2 * PATH_MAX];
                    snprintf(old_target_path, 2 * PATH_MAX, "%s/%s", data->target, original_name);

                    printf("Rename: %s -> %s\n", original_name, event->name);

                    // robienie rename w target dir
                    if (rename(old_target_path, target_path) != 0)
                    {
                        perror("rename failed");
                    }
                }
                else
                {
                    // move in
                    printf("New file/dir moved in: %s\n", event->name);
                    copy_file_or_dir(source_path, target_path);
                }
            }

            // przemieszczenie do nastepnego eventu w buforze
            i += sizeof(struct inotify_event) + event->len;
        }

        // obslugiwanie IN_MOVED_FROM ktore nigdy nie staly sie IN_MOVED_TO
        for (int j = 0; j < move_event_count; j++)
        {
            char delete_target_path[2 * PATH_MAX];
            snprintf(delete_target_path, 2 * PATH_MAX, "%s/%s", data->target, moved_events[j].name);
            printf("Unmatched MOVED_FROM - treating as deletion: %s\n", moved_events[j].name);
            delete_content(delete_target_path);
        }
        move_event_count = 0;  // czyszczenie trackera dla nastepnego cyklu eventow
    }

    for (int j = 0; j < wd_count; j++)
    {
        inotify_rm_watch(fd, wd_map[j].wd);
    }
    if (fd >= 0)
        close(fd);

    pthread_cleanup_pop(1);
    free(moved_events);

    return NULL;
}
