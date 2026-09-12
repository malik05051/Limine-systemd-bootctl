#if defined (UEFI)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <lib/smbios.h>
#include <lib/acpi.h>
#include <lib/bli.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/tpm.h>
#include <mm/pmm.h>

// Event tags shared with systemd, so that a policy written against one boot
// loader's log reads the other's.
#define SMBIOS_TYPE1_EVENT_TAG_ID  0xd5cb7cbc
#define SMBIOS_TYPE2_EVENT_TAG_ID  0xe0d47bc8
#define SMBIOS_TYPE11_EVENT_TAG_ID 0xc0b3bd23

#define SMBIOS_TYPE_END_OF_TABLE 127

struct smbios_header {
    uint8_t type;
    uint8_t length;
    uint8_t handle[2];
} __attribute__((packed));

// The wake-up type of a type 1 structure says how the machine was powered on,
// so it differs between a cold boot and a resume and would make the digest
// unreproducible.
#define SMBIOS_TYPE1_WAKE_UP_TYPE_OFFSET 24

static const uint8_t *smbios_table(size_t *size_out) {
    void *ep32 = NULL, *ep64 = NULL;
    acpi_get_smbios(&ep32, &ep64);

    if (ep64 != NULL) {
        struct smbios_entry_point_64 *ep = ep64;
        *size_out = ep->table_maximum_size;
        return (const uint8_t *)(uintptr_t)ep->table_address;
    }

    if (ep32 != NULL) {
        struct smbios_entry_point_32 *ep = ep32;
        *size_out = ep->table_length;
        return (const uint8_t *)(uintptr_t)ep->table_address;
    }

    return NULL;
}

// One structure is a fixed formatted area followed by a set of strings ending
// in a second NUL. Returns where the next structure starts, or NULL if this
// one runs past the end of the table.
static const uint8_t *smbios_structure_end(const uint8_t *p, size_t size) {
    if (size < sizeof(struct smbios_header)) {
        return NULL;
    }

    const struct smbios_header *header = (const struct smbios_header *)p;
    if (size < header->length || header->length < sizeof(struct smbios_header)) {
        return NULL;
    }

    const uint8_t *q = p + header->length;
    size -= header->length;

    // A structure with no strings at all still carries the two terminators.
    if (size >= 2 && q[0] == 0 && q[1] == 0) {
        return q + 2;
    }

    bool first = true;
    for (;;) {
        const uint8_t *e = memchr(q, 0, size);
        if (e == NULL) {
            return NULL;
        }

        if (!first && e == q) {
            return q + 1;
        }

        size -= (size_t)(e + 1 - q);
        q = e + 1;
        first = false;
    }
}

static void smbios_measure_type1(const struct smbios_header *header, size_t size,
                                 bool *measured) {
    const void *p = header;
    uint8_t *copy = NULL;

    // Whether the field exists at all is governed by the formatted area's
    // length, which comes from firmware and cannot be assumed.
    if (header->length > SMBIOS_TYPE1_WAKE_UP_TYPE_OFFSET) {
        copy = ext_mem_alloc(size);
        memcpy(copy, header, size);
        copy[SMBIOS_TYPE1_WAKE_UP_TYPE_OFFSET] = 0;
        p = copy;
    }

    tpm_measure_tagged(TPM_PCR_PLATFORM_CONFIG, SMBIOS_TYPE1_EVENT_TAG_ID,
                       p, size, "smbios:type1");
    *measured = true;

    if (copy != NULL) {
        pmm_free(copy, size);
    }
}

void smbios_measure(void) {
    if (!measured_boot) {
        return;
    }

    // Re-extending PCR 1 would invalidate a value something earlier in the
    // chain already published.
    if (bli_smbios_pcr_recorded()) {
        return;
    }

    size_t size;
    const uint8_t *p = smbios_table(&size);
    if (p == NULL) {
        return;
    }

    bool measured = false;

    for (;;) {
        if (size < sizeof(struct smbios_header)) {
            break;
        }

        const struct smbios_header *header = (const struct smbios_header *)p;

        if (header->type == SMBIOS_TYPE_END_OF_TABLE) {
            break;
        }

        const uint8_t *next = smbios_structure_end(p, size);
        if (next == NULL) {
            break;
        }

        size_t structure_size = (size_t)(next - p);

        switch (header->type) {
            case 1: {
                smbios_measure_type1(header, structure_size, &measured);
                break;
            }
            case 2: {
                tpm_measure_tagged(TPM_PCR_PLATFORM_CONFIG, SMBIOS_TYPE2_EVENT_TAG_ID,
                                   header, structure_size, "smbios:type2");
                measured = true;
                break;
            }
            case 11: {
                tpm_measure_tagged(TPM_PCR_PLATFORM_CONFIG, SMBIOS_TYPE11_EVENT_TAG_ID,
                                   header, structure_size, "smbios:type11");
                measured = true;
                break;
            }
            default: {
                break;
            }
        }

        size -= structure_size;
        p = next;
    }

    if (measured) {
        bli_set_smbios_pcr(TPM_PCR_PLATFORM_CONFIG);
    }
}

#endif
