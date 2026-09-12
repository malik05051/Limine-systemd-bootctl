#ifndef FS__FAT32_H__
#define FS__FAT32_H__

#include <stdbool.h>
#include <lib/part.h>
#include <fs/file.h>

char *fat32_get_label(struct volume *part);

struct file_handle *fat32_open(struct volume *part, const char *path);

bool fat32_readdir(struct volume *part, const char *path,
                   bool (*callback)(const char *name, bool is_dir, void *ctx),
                   void *ctx);

#endif
