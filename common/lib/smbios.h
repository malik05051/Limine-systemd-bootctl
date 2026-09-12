#ifndef LIB__SMBIOS_H__
#define LIB__SMBIOS_H__

#if defined (UEFI)

// Measure the SMBIOS structures describing the machine into PCR 1, and record
// which PCR was used so nothing downstream measures them a second time.
void smbios_measure(void);

#endif

#endif
