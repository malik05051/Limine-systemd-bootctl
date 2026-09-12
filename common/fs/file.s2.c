#include <stddef.h>
#include <stdint.h>
#include <fs/file.h>
#include <fs/fat32.h>
#include <fs/iso9660.h>
#include <lib/print.h>
#include <lib/misc.h>
#include <mm/pmm.h>
#include <lib/part.h>
#include <lib/libc.h>
#include <pxe/tftp.h>

char *fs_get_label(struct volume *part) {
    char *ret;

    if ((ret = fat32_get_label(part)) != NULL) {
        return ret;
    }

    if ((ret = iso9660_get_label(part)) != NULL) {
        return ret;
    }

    return NULL;
}

bool fs_get_guid(struct guid *guid, struct volume *part) {
    (void)guid; (void)part;

    return false;
}

bool case_insensitive_fopen = false;

bool fs_readdir(struct volume *part, const char *path,
                bool (*callback)(const char *name, bool is_dir, void *ctx),
                void *ctx) {
    if (part->pxe) {
        return false;
    }

    return fat32_readdir(part, path, callback, ctx);
}

#if defined (UEFI)

#define FS_RENAME_NAME_MAX 256

// The firmware wants a backslash-separated path in UTF-16.
static bool fs_to_efi_path(CHAR16 *out, size_t out_size, const char *path) {
    size_t pos = 0;

    if (path[0] != '/') {
        if (pos >= out_size - 1) {
            return false;
        }
        out[pos++] = L'\\';
    }

    for (; *path != '\0'; path++) {
        if (pos >= out_size - 1) {
            return false;
        }
        if ((unsigned char)*path > 0x7f) {
            return false;
        }
        out[pos++] = *path == '/' ? L'\\' : (CHAR16)*path;
    }

    out[pos] = L'\0';
    return true;
}

bool fs_rename(struct volume *part, const char *path, const char *new_name) {
    if (part->pxe || part->efi_part_handle == NULL) {
        return false;
    }

    size_t name_len = strlen(new_name);
    if (name_len == 0 || name_len >= FS_RENAME_NAME_MAX) {
        return false;
    }

    CHAR16 wide_path[512];
    if (!fs_to_efi_path(wide_path, SIZEOF_ARRAY(wide_path), path)) {
        return false;
    }

    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    if (gBS->HandleProtocol(part->efi_part_handle, &fs_guid, (void **)&fs) != EFI_SUCCESS) {
        return false;
    }

    EFI_FILE_HANDLE root = NULL;
    if (fs->OpenVolume(fs, &root) != EFI_SUCCESS) {
        return false;
    }

    EFI_FILE_HANDLE file = NULL;
    EFI_STATUS status = root->Open(root, &file, wide_path,
                                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    root->Close(root);

    if (status != EFI_SUCCESS) {
        return false;
    }

    // SetInfo takes a whole EFI_FILE_INFO back, so the existing one is read
    // first and only its name replaced.
    EFI_GUID info_guid = EFI_FILE_INFO_ID;
    size_t info_size = SIZE_OF_EFI_FILE_INFO + FS_RENAME_NAME_MAX * sizeof(CHAR16);
    EFI_FILE_INFO *info = ext_mem_alloc(info_size);

    UINTN size = info_size;
    bool ok = false;

    if (file->GetInfo(file, &info_guid, &size, info) == EFI_SUCCESS) {
        for (size_t i = 0; i < name_len; i++) {
            if ((unsigned char)new_name[i] > 0x7f) {
                goto out;
            }
            info->FileName[i] = new_name[i];
        }
        info->FileName[name_len] = L'\0';

        size = SIZE_OF_EFI_FILE_INFO + (name_len + 1) * sizeof(CHAR16);
        info->Size = size;

        ok = file->SetInfo(file, &info_guid, size, info) == EFI_SUCCESS;
    }

out:
    pmm_free(info, info_size);
    file->Close(file);

    return ok;
}

#endif

struct file_handle *fopen(struct volume *part, const char *filename) {
    size_t filename_new_len = strlen(filename) + 2;
    char *filename_new = ext_mem_alloc(filename_new_len);

    if (filename[0] != '/') {
        filename_new[0] = '/';
        strcpy(&filename_new[1], filename);
    } else {
        strcpy(filename_new, filename);
    }

    filename = filename_new;

    struct file_handle *ret;

    if (part->pxe) {
        if ((ret = tftp_open(part, "", filename)) == NULL) {
            goto err;
        }
        pmm_free(filename_new, filename_new_len);
        return ret;
    }

    if ((ret = iso9660_open(part, filename)) != NULL) {
        goto success;
    }
    if ((ret = fat32_open(part, filename)) != NULL) {
        goto success;
    }

err:
    pmm_free(filename_new, filename_new_len);
    return NULL;

success:
    ret->path = (char *)filename;
    ret->path_len = filename_new_len;

    return ret;
}

void fclose(struct file_handle *fd) {
    if (fd->is_memfile) {
        if (fd->readall == false) {
            pmm_free(fd->fd, fd->size);
        }
    } else {
        fd->close(fd);
    }
    pmm_free(fd->path, fd->path_len);
    pmm_free(fd, sizeof(struct file_handle));
}

uint64_t fread(struct file_handle *fd, void *buf, uint64_t loc, uint64_t count) {
    if (loc > fd->size || count > fd->size - loc) {
        panic(false, "fread: attempted out of bounds read");
    }

    if (fd->is_memfile) {
#if defined (__i386__)
        if (fd->is_high_mem) {
            panic(false, "fread: memfile resides above 4 GiB; caller must use load_addr_64 directly");
        }
#endif
        memcpy(buf, fd->fd + loc, count);
        return count;
    } else {
        return fd->read(fd, buf, loc, count);
    }
}
