#ifndef LIB__UKI_H__
#define LIB__UKI_H__

#if defined (UEFI)

#include <stddef.h>

#define UKI_PROFILES_MAX 64

struct uki_profile {
    char id[64];
    char title[64];
};

// Profiles @1 onwards of the multi-profile UKI at `uri`, in order: booting
// it with no selector already gives @0. Read from the file unverified, so
// fit only for labelling menu entries.
size_t uki_extra_profiles(char *uri, struct uki_profile *out, size_t max);

#endif

#endif
