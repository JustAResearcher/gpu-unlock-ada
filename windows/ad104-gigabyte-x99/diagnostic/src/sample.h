/* SPDX-License-Identifier: MIT */
#ifndef AD104_SAMPLE_H
#define AD104_SAMPLE_H
#include "core.h"
#include <Protocol/PciIo.h>
BOOLEAN ResourceMatches(const UINT8 *descriptor,UINT32 raw,UINT64 base,UINT64 *size);
UINTN SampleRegisters(TEXT *text,EFI_PCI_IO_PROTOCOL *pci,EFI_BOOT_SERVICES *bs);
UINTN SampleProm(TEXT *text,EFI_PCI_IO_PROTOCOL *pci,EFI_BOOT_SERVICES *bs);
#endif
