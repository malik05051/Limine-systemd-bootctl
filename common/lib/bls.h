#ifndef LIB__BLS_H__
#define LIB__BLS_H__

// Discover Type #1 boot loader entries under /loader/entries on the boot
// volume and append them to the menu tree, ordered as the Boot Loader
// Specification requires. Entries described by limine.conf take precedence: a
// snippet naming a kernel an existing entry already boots is dropped.
void bls_append_entries(void);

#if defined (UEFI)

// Record a boot attempt against a Type #1 entry carrying a boot counter,
// renaming its snippet so the count survives a boot that never completes.
// Entries without a counter are left alone.
void bls_count_boot(const char *entry_id);

#endif

#endif
