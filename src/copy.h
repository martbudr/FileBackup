#ifndef COPY_H
#define COPY_H

void copy_file(char* src, char* dst);

void copy_symlink(char* src_root, char* dst_root, char* rel_path);

void copy_recursive(char* src_root, char* dst_root, char* rel_path);

#endif
