#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/bls.h>
#include <lib/config.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <fs/file.h>
#include <lib/guid.h>
#include <lib/part.h>
#include <mm/pmm.h>
#include <menu.h>

#define BLS_DIR "/loader/entries"
#define BLS_MAX_ENTRIES 256
#define BLS_MAX_SNIPPET (64 * 1024)
#define BLS_MAX_INITRDS 8
#define BLS_MAX_PREFIX 64
#define BLS_MAX_COUNTED 64

// The Extended Boot Loader partition, which holds boot entries alongside, or
// instead of, the ones on the EFI system partition.
#define XBOOTLDR_TYPE_GUID "bc13c2ff-59e6-4262-a352-b275fd6f7172"

struct bls_source {
    struct volume *vol;
    char prefix[BLS_MAX_PREFIX];
};

struct bls_snippet {
    const struct bls_source *source;
    char *body;
    char *id;
    // Boot counting, from a "+left-done" suffix on the filename.
    bool counted;
    unsigned tries_left;
    unsigned tries_done;
    char *title;
    char *version;
    char *sort_key;
    char *machine_id;
    char *linux_path;
    char *efi_path;
    char *devicetree;
    char *options;
    char *initrd[BLS_MAX_INITRDS];
    size_t initrd_count;
};

struct bls_names {
    char *name[BLS_MAX_ENTRIES];
    size_t count;
    bool truncated;
};

static char *bls_strdup(const char *s, size_t len) {
    char *r = ext_mem_alloc(len + 1);
    memcpy(r, s, len);
    r[len] = '\0';
    return r;
}

static bool bls_has_conf_suffix(const char *name) {
    size_t len = strlen(name);

    if (len <= 5) {
        return false;
    }

    return strcasecmp(name + len - 5, ".conf") == 0;
}

static bool bls_collect_name(const char *name, bool is_dir, void *ctx) {
    struct bls_names *names = ctx;

    if (is_dir || !bls_has_conf_suffix(name)) {
        return true;
    }

    if (names->count == BLS_MAX_ENTRIES) {
        names->truncated = true;
        return false;
    }

    names->name[names->count++] = bls_strdup(name, strlen(name));
    return true;
}

// A counted entry's filename ends in "+<left>" or "+<left>-<done>", giving
// how many attempts remain and how many have already been made.
static void bls_parse_counter(struct bls_snippet *snippet) {
    const char *plus = NULL;

    for (const char *p = snippet->id; *p != '\0'; p++) {
        if (*p == '+') {
            plus = p;
        }
    }

    if (plus == NULL || plus[1] == '\0') {
        return;
    }

    const char *p = plus + 1;
    unsigned left = 0;
    size_t digits = 0;
    while (*p >= '0' && *p <= '9') {
        left = left * 10 + (unsigned)(*p++ - '0');
        digits++;
    }

    if (digits == 0 || digits > 9) {
        return;
    }

    unsigned done = 0;
    if (*p == '-') {
        p++;
        digits = 0;
        while (*p >= '0' && *p <= '9') {
            done = done * 10 + (unsigned)(*p++ - '0');
            digits++;
        }
        if (digits == 0 || digits > 9) {
            return;
        }
    }

    if (*p != '\0') {
        return;
    }

    snippet->counted = true;
    snippet->tries_left = left;
    snippet->tries_done = done;
}

// Compare two version strings the way a reader would: digit runs by value, so
// that 6.10 sorts above 6.9, and everything else byte by byte.
static int bls_vercmp(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            while (*a == '0') {
                a++;
            }
            while (*b == '0') {
                b++;
            }

            size_t da = 0, db = 0;
            while (a[da] >= '0' && a[da] <= '9') {
                da++;
            }
            while (b[db] >= '0' && b[db] <= '9') {
                db++;
            }

            if (da != db) {
                return da < db ? -1 : 1;
            }

            int digits = strncmp(a, b, da);
            if (digits != 0) {
                return digits < 0 ? -1 : 1;
            }

            a += da;
            b += db;
            continue;
        }

        if (*a != *b) {
            return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
        }

        a++;
        b++;
    }

    if (*a == *b) {
        return 0;
    }

    return *a == '\0' ? -1 : 1;
}

static int bls_strcmp_null(const char *a, const char *b) {
    // An absent field sorts after a present one, as the specification says of
    // sort-key.
    if (a == NULL || b == NULL) {
        if (a == b) {
            return 0;
        }
        return a == NULL ? 1 : -1;
    }

    return strcmp(a, b);
}

// Specification order: by sort-key, then machine-id, then version with the
// newest first, then the filename as a tie-break.
static int bls_compare(const struct bls_snippet *a, const struct bls_snippet *b) {
    bool a_spent = a->counted && a->tries_left == 0;
    bool b_spent = b->counted && b->tries_left == 0;

    if (a_spent != b_spent) {
        return a_spent ? 1 : -1;
    }

    int r = bls_strcmp_null(a->sort_key, b->sort_key);
    if (r != 0) {
        return r;
    }

    r = bls_strcmp_null(a->machine_id, b->machine_id);
    if (r != 0) {
        return r;
    }

    if (a->version != NULL && b->version != NULL) {
        r = bls_vercmp(a->version, b->version);
        if (r != 0) {
            return -r;
        }
    } else if (a->version != b->version) {
        return a->version == NULL ? 1 : -1;
    }

    return strcmp(a->id, b->id);
}

static void bls_store_key(struct bls_snippet *snippet, const char *key, size_t key_len,
                          const char *value, size_t value_len) {
    struct {
        const char *name;
        char **field;
    } fields[] = {
        { "title",       &snippet->title      },
        { "version",     &snippet->version    },
        { "sort-key",    &snippet->sort_key   },
        { "machine-id",  &snippet->machine_id },
        { "linux",       &snippet->linux_path },
        { "efi",         &snippet->efi_path   },
        { "devicetree",  &snippet->devicetree },
        { "options",     &snippet->options    },
    };

    for (size_t i = 0; i < SIZEOF_ARRAY(fields); i++) {
        if (strlen(fields[i].name) != key_len
         || strncmp(key, fields[i].name, key_len) != 0) {
            continue;
        }

        // A repeated key keeps the first value, as the specification says.
        if (*fields[i].field == NULL) {
            *fields[i].field = bls_strdup(value, value_len);
        }
        return;
    }

    // initrd is the one key that may legitimately repeat.
    if (key_len == 6 && strncmp(key, "initrd", 6) == 0
     && snippet->initrd_count < BLS_MAX_INITRDS) {
        snippet->initrd[snippet->initrd_count++] = bls_strdup(value, value_len);
    }
}

static void bls_parse(struct bls_snippet *snippet, const char *text, size_t size) {
    size_t i = 0;

    while (i < size) {
        size_t line_end = i;
        while (line_end < size && text[line_end] != '\n' && text[line_end] != '\r') {
            line_end++;
        }

        size_t p = i;
        while (p < line_end && (text[p] == ' ' || text[p] == '\t')) {
            p++;
        }

        if (p < line_end && text[p] != '#') {
            size_t key_start = p;
            while (p < line_end && text[p] != ' ' && text[p] != '\t') {
                p++;
            }
            size_t key_len = p - key_start;

            while (p < line_end && (text[p] == ' ' || text[p] == '\t')) {
                p++;
            }

            size_t value_end = line_end;
            while (value_end > p
                && (text[value_end - 1] == ' ' || text[value_end - 1] == '\t')) {
                value_end--;
            }

            if (key_len > 0 && value_end > p) {
                bls_store_key(snippet, text + key_start, key_len, text + p, value_end - p);
            }
        }

        i = line_end;
        while (i < size && (text[i] == '\n' || text[i] == '\r')) {
            i++;
        }
    }
}

static char *bls_read_snippet(struct volume *vol, const char *name, size_t *size_out) {
    char path[MENU_PATH_MAX];
    size_t pos = 0;

    const char *dir = BLS_DIR "/";
    while (*dir != '\0' && pos < sizeof(path) - 1) {
        path[pos++] = *dir++;
    }
    while (*name != '\0' && pos < sizeof(path) - 1) {
        path[pos++] = *name++;
    }
    path[pos] = '\0';

    if (*name != '\0') {
        return NULL;
    }

    struct file_handle *f = fopen(vol, path);
    if (f == NULL) {
        return NULL;
    }

    if (f->size == 0 || f->size > BLS_MAX_SNIPPET) {
        fclose(f);
        return NULL;
    }

    size_t size = f->size;
    char *text = ext_mem_alloc(size);

    if (fread(f, text, 0, size) != size) {
        pmm_free(text, size);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *size_out = size;
    return text;
}

struct bls_body {
    char *buf;
    size_t size;
    size_t pos;
    bool overflow;
};

static void bls_body_append(struct bls_body *body, const char *s) {
    while (*s != '\0') {
        if (body->pos >= body->size - 1) {
            body->overflow = true;
            return;
        }
        body->buf[body->pos++] = *s++;
    }
    body->buf[body->pos] = '\0';
}

// A Type #1 path is absolute on the partition holding the entries, which is
// the one the config was read from.
static void bls_body_append_path(struct bls_body *body, const char *prefix, const char *path) {
    bls_body_append(body, prefix);
    if (path[0] != '/') {
        bls_body_append(body, "/");
    }
    bls_body_append(body, path);
}

static char *bls_build_body(const struct bls_snippet *snippet) {
    const char *prefix = snippet->source->prefix;
    size_t size = 4096;
    struct bls_body body = { .buf = ext_mem_alloc(size), .size = size, .pos = 0 };

    body.buf[0] = '\0';

    if (snippet->efi_path != NULL) {
        bls_body_append(&body, "PROTOCOL: efi\nPATH: ");
        bls_body_append_path(&body, prefix, snippet->efi_path);
        bls_body_append(&body, "\n");
    } else {
        bls_body_append(&body, "PROTOCOL: linux\nKERNEL_PATH: ");
        bls_body_append_path(&body, prefix, snippet->linux_path);
        bls_body_append(&body, "\n");

        for (size_t i = 0; i < snippet->initrd_count; i++) {
            bls_body_append(&body, "MODULE_PATH: ");
            bls_body_append_path(&body, prefix, snippet->initrd[i]);
            bls_body_append(&body, "\n");
        }

        if (snippet->devicetree != NULL) {
            bls_body_append(&body, "DTB_PATH: ");
            bls_body_append_path(&body, prefix, snippet->devicetree);
            bls_body_append(&body, "\n");
        }
    }

    if (snippet->options != NULL) {
        bls_body_append(&body, "CMDLINE: ");
        bls_body_append(&body, snippet->options);
        bls_body_append(&body, "\n");
    }

    if (body.overflow) {
        pmm_free(body.buf, size);
        return NULL;
    }

    return body.buf;
}

static char *bls_build_name(const struct bls_snippet *snippet) {
    const char *title = snippet->title != NULL ? snippet->title : snippet->id;

    size_t len = strlen(title);
    if (snippet->version != NULL) {
        len += strlen(snippet->version) + 3;
    }

    char *name = ext_mem_alloc(len + 1);
    size_t pos = 0;

    for (const char *s = title; *s != '\0'; s++) {
        name[pos++] = *s;
    }

    if (snippet->version != NULL) {
        name[pos++] = ' ';
        name[pos++] = '(';
        for (const char *s = snippet->version; *s != '\0'; s++) {
            name[pos++] = *s;
        }
        name[pos++] = ')';
    }

    name[pos] = '\0';
    return name;
}

// The path an entry boots, with any volume specifier removed, so that a
// limine.conf entry and a snippet naming the same file compare equal.
static const char *bls_bare_path(const char *path) {
    const char *colon = strchr(path, ':');
    return colon != NULL ? colon + 1 : path;
}

static bool bls_kernel_already_listed(struct menu_entry *node, const char *prefix,
                                      const char *path) {
    // Only a snippet on the volume the config came from may be matched on its
    // path alone: elsewhere the same path names a different file.
    bool bare_ok = strcmp(prefix, "boot():") == 0;

    for (; node != NULL; node = node->next) {
        if (node->sub != NULL) {
            if (bls_kernel_already_listed(node->sub, prefix, path)) {
                return true;
            }
            continue;
        }

        if (node->body == NULL) {
            continue;
        }

        char *existing = config_get_value(node->body, 0, "KERNEL_PATH");
        if (existing == NULL) {
            existing = config_get_value(node->body, 0, "PATH");
        }
        if (existing == NULL) {
            continue;
        }

        if (bare_ok) {
            if (strcasecmp(bls_bare_path(existing), bls_bare_path(path)) == 0) {
                return true;
            }
        } else {
            size_t prefix_len = strlen(prefix);
            if (strncasecmp(existing, prefix, prefix_len) == 0
             && strcasecmp(bls_bare_path(existing + prefix_len), bls_bare_path(path)) == 0) {
                return true;
            }
        }
    }

    return false;
}

static bool bls_is_xbootldr(struct volume *vol) {
    struct guid xbootldr;

    if (!vol->part_type_guid_valid) {
        return false;
    }

    if (!string_to_guid_mixed(&xbootldr, XBOOTLDR_TYPE_GUID)) {
        return false;
    }

    return memcmp(&vol->part_type_guid, &xbootldr, sizeof(struct guid)) == 0;
}

// Paths in a snippet are relative to the partition holding it, so a partition
// other than the one the config came from has to be named outright.
static bool bls_source_prefix(struct volume *vol, char *prefix) {
    if (!vol->part_guid_valid) {
        return false;
    }

    char guid_str[37];
    guid_to_string(&vol->part_guid, guid_str);

    size_t pos = 0;
    for (const char *p = "guid("; *p != '\0'; p++) {
        prefix[pos++] = *p;
    }
    for (size_t i = 0; guid_str[i] != '\0'; i++) {
        prefix[pos++] = guid_str[i];
    }
    prefix[pos++] = ')';
    prefix[pos++] = ':';
    prefix[pos] = '\0';

    return true;
}

static size_t bls_gather(const struct bls_source *source,
                         struct bls_snippet *snippets, size_t max) {
    struct bls_names names = {0};
    size_t count = 0;

    if (!fs_readdir(source->vol, BLS_DIR, bls_collect_name, &names)) {
        return 0;
    }

    if (names.truncated) {
        printv("bls: more than %u entries in %s, ignoring the rest\n",
               BLS_MAX_ENTRIES, BLS_DIR);
    }

    for (size_t i = 0; i < names.count && count < max; i++) {
        size_t size;
        char *text = bls_read_snippet(source->vol, names.name[i], &size);
        if (text == NULL) {
            printv("bls: could not read %s\n", names.name[i]);
            pmm_free(names.name[i], strlen(names.name[i]) + 1);
            names.name[i] = NULL;
            continue;
        }

        struct bls_snippet *snippet = &snippets[count];
        memset(snippet, 0, sizeof(struct bls_snippet));
        snippet->source = source;
        snippet->id = bls_strdup(names.name[i], strlen(names.name[i]) - 5);

        bls_parse_counter(snippet);
        bls_parse(snippet, text, size);
        pmm_free(text, size);
        pmm_free(names.name[i], strlen(names.name[i]) + 1);
        names.name[i] = NULL;

        if (snippet->linux_path == NULL && snippet->efi_path == NULL) {
            printv("bls: %s names no kernel, skipping\n", snippet->id);
            continue;
        }

        count++;
    }

    return count;
}

#define BLS_COUNTED_NAME_MAX 128

struct bls_counted {
    struct volume *vol;
    const char *id;
    // What the snippet is called on disk right now, which stops matching the
    // id the moment the counter is written down.
    char name[BLS_COUNTED_NAME_MAX];
    unsigned tries_left;
    unsigned tries_done;
};

static struct bls_counted bls_counted_entries[BLS_MAX_COUNTED];
static size_t bls_counted_count = 0;

static void bls_remember_counter(const struct bls_snippet *snippet) {
    if (!snippet->counted || bls_counted_count == BLS_MAX_COUNTED) {
        return;
    }

    size_t len = strlen(snippet->id);
    if (len >= BLS_COUNTED_NAME_MAX) {
        return;
    }

    struct bls_counted *counted = &bls_counted_entries[bls_counted_count++];
    counted->vol = snippet->source->vol;
    counted->id = snippet->id;
    memcpy(counted->name, snippet->id, len + 1);
    counted->tries_left = snippet->tries_left;
    counted->tries_done = snippet->tries_done;
}

static size_t bls_append_uint(char *buf, size_t pos, unsigned value) {
    char digits[10];
    size_t ndigits = 0;

    do {
        digits[ndigits++] = '0' + (char)(value % 10);
        value /= 10;
    } while (value > 0);

    while (ndigits > 0) {
        buf[pos++] = digits[--ndigits];
    }

    return pos;
}

void bls_count_boot(const char *entry_id) {
    if (entry_id == NULL) {
        return;
    }

    for (size_t i = 0; i < bls_counted_count; i++) {
        struct bls_counted *counted = &bls_counted_entries[i];

        if (strcmp(counted->id, entry_id) != 0) {
            continue;
        }

        // Out of attempts already: the entry still boots if chosen, but there
        // is nothing left to count down.
        if (counted->tries_left == 0) {
            return;
        }

        char old_path[MENU_PATH_MAX];
        size_t pos = 0;
        for (const char *p = BLS_DIR "/"; *p != '\0'; p++) {
            old_path[pos++] = *p;
        }
        for (const char *p = counted->name; *p != '\0'; p++) {
            old_path[pos++] = *p;
        }
        for (const char *p = ".conf"; *p != '\0'; p++) {
            old_path[pos++] = *p;
        }
        old_path[pos] = '\0';

        // The base name is everything before the counter.
        // Room for the longest counter the parser accepts on top of the base.
        char new_name[BLS_COUNTED_NAME_MAX + 32];
        size_t base_len = 0;
        for (size_t j = 0; counted->name[j] != '\0'; j++) {
            if (counted->name[j] == '+') {
                base_len = j;
            }
        }

        if (base_len == 0) {
            return;
        }

        memcpy(new_name, counted->name, base_len);
        pos = base_len;
        new_name[pos++] = '+';
        pos = bls_append_uint(new_name, pos, counted->tries_left - 1);
        new_name[pos++] = '-';
        pos = bls_append_uint(new_name, pos, counted->tries_done + 1);
        for (const char *p = ".conf"; *p != '\0'; p++) {
            new_name[pos++] = *p;
        }
        new_name[pos] = '\0';

#if defined (UEFI)
        if (fs_rename(counted->vol, old_path, new_name)) {
            counted->tries_left--;
            counted->tries_done++;
            // The rename may have lengthened the counter; the entry simply
            // stops being counted if the new name no longer fits.
            new_name[pos - 5] = '\0';
            if (pos - 4 <= BLS_COUNTED_NAME_MAX) {
                memcpy(counted->name, new_name, pos - 4);
            } else {
                counted->tries_left = 0;
            }
        } else
#endif
        {
            // Booting an entry whose count could not be written down is
            // better than not booting, but the attempt is now invisible.
            printv("bls: could not count the boot of %s\n", counted->id);
        }

        return;
    }
}

void bls_append_entries(void) {
    if (boot_volume == NULL || boot_volume->pxe) {
        return;
    }

    // With a config hash enrolled, everything booted has to be named by the
    // config that hash authenticates, with its own hash beside it. A snippet
    // is neither, so honouring one would let anything that can write to the
    // ESP boot what it likes through a signed loader.
    if (secure_boot_active) {
        printv("bls: config is enrolled, not reading %s\n", BLS_DIR);
        return;
    }

    struct bls_source sources[2];
    size_t source_count = 1;

    sources[0].vol = boot_volume;
    strcpy(sources[0].prefix, "boot():");

    volume_iterate_parts(boot_volume,
        if (_PART == boot_volume || !bls_is_xbootldr(_PART)) {
            continue;
        }

        if (!bls_source_prefix(_PART, sources[1].prefix)) {
            printv("bls: the XBOOTLDR partition has no GUID to address it by\n");
            break;
        }

        sources[1].vol = _PART;
        source_count = 2;
        break;
    );

    size_t alloc_size = BLS_MAX_ENTRIES * sizeof(struct bls_snippet);
    struct bls_snippet *snippets = ext_mem_alloc(alloc_size);
    size_t count = 0;

    for (size_t i = 0; i < source_count; i++) {
        count += bls_gather(&sources[i], snippets + count, BLS_MAX_ENTRIES - count);
    }

    // Insertion sort: the list is short and this keeps equal entries in the
    // order the directory gave them.
    for (size_t i = 1; i < count; i++) {
        struct bls_snippet key = snippets[i];
        size_t j = i;
        while (j > 0 && bls_compare(&snippets[j - 1], &key) > 0) {
            snippets[j] = snippets[j - 1];
            j--;
        }
        snippets[j] = key;
    }

    // Decided against the menu as limine.conf left it, before anything is
    // appended: two snippets may legitimately share a kernel and differ only
    // in their initrd or options, and neither displaces the other.
    for (size_t i = 0; i < count; i++) {
        struct bls_snippet *snippet = &snippets[i];

        const char *path = snippet->efi_path != NULL ? snippet->efi_path : snippet->linux_path;
        if (bls_kernel_already_listed(menu_tree, snippet->source->prefix, path)) {
            printv("bls: %s is already in limine.conf, skipping\n", snippet->id);
            continue;
        }

        snippet->body = bls_build_body(snippet);
        if (snippet->body == NULL) {
            printv("bls: %s does not fit in an entry, skipping\n", snippet->id);
        }
    }

    struct menu_entry **tail = &menu_tree;
    while (*tail != NULL) {
        tail = &(*tail)->next;
    }

    for (size_t i = 0; i < count; i++) {
        struct bls_snippet *snippet = &snippets[i];

        if (snippet->body == NULL) {
            continue;
        }

        bls_remember_counter(snippet);

        struct menu_entry *entry = ext_mem_alloc(sizeof(struct menu_entry));
        entry->name = bls_build_name(snippet);
        entry->body = snippet->body;
        entry->bli_id = snippet->id;

        *tail = entry;
        tail = &entry->next;
    }

    pmm_free(snippets, alloc_size);
}
