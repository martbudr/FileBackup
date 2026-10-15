# sop-backup

An interactive directory backup tool written in C17 for Linux. It copies a source directory to one or more destinations, then uses `inotify` and a watcher thread per destination to propagate filesystem changes while the program runs.

## Build and run

Requires Linux, Make, and a C17 compiler with POSIX threads and sanitizer support. The Linux `inotify` dependency prevents native macOS builds.

```sh
make
./sop-backup
```

The default build enables AddressSanitizer, UndefinedBehaviorSanitizer, and LeakSanitizer. `make CI=1` builds without sanitizers and treats warnings as errors. Use `make clean` to remove the executable and object files.

## Commands

Enter commands at the program's prompt:

| Command | Effect |
| --- | --- |
| `add <source> <targets...>` | Recursively copy the source to each destination and start watching. Destinations must be empty or not yet exist; they cannot be the source or lie inside it. |
| `end <source> <targets...>` | Stop the specified backups, leaving copied files in place. |
| `restore <source> <target>` | Make the target match the source, copying changed files and deleting target entries absent from the source, then start watching. The target may contain files; neither directory may contain the other. |
| `list` | Show registered sources and their active destinations. |
| `exit` | Stop all watchers and exit. |

For `restore`, the first path is the directory to copy **from** (such as a saved backup), and the second is the directory to restore **into**. Existing target files can be overwritten or deleted.

Example session, assuming `documents` exists and the backup destinations are new or empty:

```text
add documents backup-a backup-b
list
end documents backup-a backup-b
restore backup-a recovered-documents
exit
```

Quote paths containing spaces, such as `add "my documents" "backup copy"`. Use the same path spelling when referring to an existing backup in later commands.

## Behavior and scope

- Initial copying handles regular files, directories, and symbolic links. Absolute links pointing inside the source are rewritten toward the corresponding backup path.
- Watchers react to creation, modification, permission changes, deletion, and move events. Source deletions can remove destination files: these are live mirrors with no version history.
- Backup registrations last only for the current session. Restarting the program does not resume them automatically.
- Each watcher tracks at most 1,024 directories and 100 pending move events. Complex moves and live symbolic-link updates have limited handling.

The implementation lives in `src/`: `main.c` and `parser.c` handle commands, `backup.c` manages backups, `watcher_thread.c` monitors changes, and `copy.c`, `restore.c`, and `utils.c` implement filesystem operations.

## License

[MIT](LICENSE).

