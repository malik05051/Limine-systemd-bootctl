#ifndef LIB__PE_H__
#define LIB__PE_H__

#include <stdint.h>
#include <stdbool.h>
#include <lib/misc.h>

int pe_bits(uint8_t *image, size_t image_size);

bool pe64_load(uint8_t *image, size_t file_size, uint64_t *entry_point, uint64_t *_slide, uint32_t alloc_type, bool kaslr, struct mem_range **ranges, uint64_t *ranges_count, uint64_t *physical_base, uint64_t *virtual_base, uint64_t *image_size, uint64_t *image_size_before_bss, bool *is_reloc);

#if defined (UEFI)

#include <fs/file.h>

struct pe_section {
    char name[9];
    uint64_t offset;
    uint32_t size;
};

// The section table of a PE image, read without loading or verifying the
// image, so every field is whatever the file claims. A section whose data
// lies outside the file has a size of 0. The array is count entries long,
// for the caller to pmm_free().
size_t pe_file_sections(struct file_handle *fd, struct pe_section **out);

#endif

#endif
