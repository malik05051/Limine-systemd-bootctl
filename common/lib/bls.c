#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/bls.h>
#include <lib/config.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <fs/file.h>
#include <mm/pmm.h>
#include <menu.h>

#define BLS_DIR "/loader/entries"
#define BLS_MAX_ENTRIES 256
#define BLS_MAX_SNIPPET (64 * 1024)
#define BLS_MAX_INITRDS 8

struct bls_snippet {
    char *id;
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

static char *bls_read_snippet(const char *name, size_t *size_out) {
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

    struct file_handle *f = fopen(boot_volume, path);
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
static void bls_body_append_path(struct bls_body *body, const char *path) {
    bls_body_append(body, "boot():");
    if (path[0] != '/') {
        bls_body_append(body, "/");
    }
    bls_body_append(body, path);
}

static char *bls_build_body(const struct bls_snippet *snippet) {
    size_t size = 4096;
    struct bls_body body = { .buf = ext_mem_alloc(size), .size = size, .pos = 0 };

    body.buf[0] = '\0';

    if (snippet->efi_path != NULL) {
        bls_body_append(&body, "PROTOCOL: efi\nPATH: ");
        bls_body_append_path(&body, snippet->efi_path);
        bls_body_append(&body, "\n");
    } else {
        bls_body_append(&body, "PROTOCOL: linux\nKERNEL_PATH: ");
        bls_body_append_path(&body, snippet->linux_path);
        bls_body_append(&body, "\n");

        for (size_t i = 0; i < snippet->initrd_count; i++) {
            bls_body_append(&body, "MODULE_PATH: ");
            bls_body_append_path(&body, snippet->initrd[i]);
            bls_body_append(&body, "\n");
        }

        if (snippet->devicetree != NULL) {
            bls_body_append(&body, "DTB_PATH: ");
            bls_body_append_path(&body, snippet->devicetree);
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

static bool bls_kernel_already_listed(struct menu_entry *node, const char *path) {
    for (; node != NULL; node = node->next) {
        if (node->sub != NULL) {
            if (bls_kernel_already_listed(node->sub, path)) {
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

        if (strcasecmp(bls_bare_path(existing), bls_bare_path(path)) == 0) {
            return true;
        }
    }

    return false;
}

void bls_append_entries(void) {
    if (boot_volume == NULL || boot_volume->pxe) {
        return;
    }

    struct bls_names names = {0};

    if (!fs_readdir(boot_volume, BLS_DIR, bls_collect_name, &names)) {
        return;
    }

    if (names.truncated) {
        printv("bls: more than %u entries in %s, ignoring the rest\n",
               BLS_MAX_ENTRIES, BLS_DIR);
    }

    struct bls_snippet *snippets = ext_mem_alloc(names.count * sizeof(struct bls_snippet));
    size_t count = 0;

    for (size_t i = 0; i < names.count; i++) {
        size_t size;
        char *text = bls_read_snippet(names.name[i], &size);
        if (text == NULL) {
            printv("bls: could not read %s\n", names.name[i]);
            continue;
        }

        struct bls_snippet *snippet = &snippets[count];
        memset(snippet, 0, sizeof(struct bls_snippet));
        snippet->id = bls_strdup(names.name[i], strlen(names.name[i]) - 5);

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

    struct menu_entry **tail = &menu_tree;
    while (*tail != NULL) {
        tail = &(*tail)->next;
    }

    for (size_t i = 0; i < count; i++) {
        struct bls_snippet *snippet = &snippets[i];

        const char *path = snippet->efi_path != NULL ? snippet->efi_path : snippet->linux_path;
        if (bls_kernel_already_listed(menu_tree, path)) {
            printv("bls: %s is already in limine.conf, skipping\n", snippet->id);
            continue;
        }

        char *body = bls_build_body(snippet);
        if (body == NULL) {
            printv("bls: %s does not fit in an entry, skipping\n", snippet->id);
            continue;
        }

        struct menu_entry *entry = ext_mem_alloc(sizeof(struct menu_entry));
        entry->name = bls_build_name(snippet);
        entry->body = body;
        entry->bli_id = snippet->id;

        *tail = entry;
        tail = &entry->next;
    }

    pmm_free(snippets, names.count * sizeof(struct bls_snippet));
}
