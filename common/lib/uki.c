#if defined (UEFI)

#include <stdint.h>
#include <stddef.h>
#include <lib/uki.h>
#include <lib/uri.h>
#include <lib/pe.h>
#include <lib/libc.h>
#include <lib/print.h>
#include <fs/file.h>
#include <mm/pmm.h>

#define UKI_PROFILE_META_MAX 4096

// Copies a value in the os-release style the UAPI UKI spec gives .profile,
// dropping one pair of enclosing quotes. Only printable ASCII is kept, as it
// ends up on the menu verbatim.
static void uki_copy_value(char *dst, size_t dst_size, const char *src, size_t len) {
    if (len >= 2 && (src[0] == '"' || src[0] == '\'') && src[len - 1] == src[0]) {
        src++;
        len -= 2;
    }

    size_t o = 0;
    for (size_t i = 0; i < len && o < dst_size - 1; i++) {
        if (src[i] >= 0x20 && src[i] <= 0x7e) {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

static void uki_parse_meta(const char *meta, struct uki_profile *profile) {
    const char *line = meta;
    while (*line != '\0') {
        size_t len = 0;
        while (line[len] != '\0' && line[len] != '\n') {
            len++;
        }

        if (len > 3 && strncmp(line, "ID=", 3) == 0) {
            uki_copy_value(profile->id, sizeof(profile->id), line + 3, len - 3);
        } else if (len > 6 && strncmp(line, "TITLE=", 6) == 0) {
            uki_copy_value(profile->title, sizeof(profile->title), line + 6, len - 6);
        }

        line += len;
        if (*line == '\n') {
            line++;
        }
    }
}

size_t uki_extra_profiles(char *uri, struct uki_profile *out, size_t max) {
    struct file_handle *fd = uri_peek(uri);
    if (fd == NULL) {
        printv("uki: cannot open %#\n", uri);
        return 0;
    }

    struct pe_section *sections;
    size_t section_count = pe_file_sections(fd, &sections);
    if (section_count == 0) {
        printv("uki: %# is not a PE image\n", uri);
        fclose(fd);
        return 0;
    }

    char *meta = ext_mem_alloc(UKI_PROFILE_META_MAX + 1);
    size_t profiles = 0;
    size_t count = 0;

    // Each .profile section opens the next profile, the first opening @0.
    for (size_t i = 0; i < section_count; i++) {
        if (memcmp(sections[i].name, ".profile", 8) != 0) {
            continue;
        }
        if (profiles++ == 0) {
            continue;
        }
        if (count == max) {
            printv("uki: %# has more than %u extra profiles, ignoring the rest\n", uri, (uint32_t)max);
            break;
        }

        struct uki_profile *profile = &out[count++];
        profile->id[0] = '\0';
        profile->title[0] = '\0';

        size_t size = sections[i].size;
        if (size > UKI_PROFILE_META_MAX) {
            size = UKI_PROFILE_META_MAX;
        }
        if (size != 0 && fread(fd, meta, sections[i].offset, size) == size) {
            meta[size] = '\0';
            uki_parse_meta(meta, profile);
        }
    }

    pmm_free(meta, UKI_PROFILE_META_MAX + 1);
    pmm_free(sections, section_count * sizeof(struct pe_section));
    fclose(fd);
    return count;
}

#endif
