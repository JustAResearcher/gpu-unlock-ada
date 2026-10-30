/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UNIVERSAL_GPU_TYPES_H
#define UNIVERSAL_GPU_TYPES_H

#include "uefi_base.h"

#define NVIDIA_VENDOR_ID 0x10DE

typedef enum {
    GPU_ARCH_UNKNOWN = 0,
    GPU_ARCH_AMPERE,      /* RTX 30 Series (GA102, GA103, GA104, GA106, GA107) */
    GPU_ARCH_ADA_LOVELACE /* RTX 40 Series (AD102, AD103, AD104, AD106, AD107) */
} GPU_ARCH;

typedef struct {
    UINT16 vendor_id;
    UINT16 device_id;
    UINT16 subsys_vendor_id;
    UINT16 subsys_device_id;
    UINT8  revision_id;
    UINT32 class_code;
    
    /* PCIe Topology Location */
    UINTN segment;
    UINTN bus;
    UINTN device;
    UINTN function;
    
    /* BAR0 MMIO Window */
    UINT64 bar0_base;
    UINT64 bar0_size;
    
    /* Hardware Architecture Classification */
    GPU_ARCH arch;
    CONST CHAR16 *chip_name;
    CONST CHAR16 *model_name;
    
    /* Strict Hardware ID string: e.g., "10de_2782_1458_40e8" */
    CHAR16 rom_filename[64];
    
    /* Loaded VBIOS buffer in RAM (never written to SPI flash) */
    VOID  *vbios_data;
    UINTN  vbios_size;
    BOOLEAN vbios_loaded;
    
    /* State */
    EFI_HANDLE handle;
    EFI_PCI_IO_PROTOCOL *pci_io;
    BOOLEAN is_display_adapter;  /* Has UEFI GOP driver bound (outputs display) */
    BOOLEAN is_console_display;  /* Specifically driving active UEFI text/graphics console */
    BOOLEAN unlock_selected;
    BOOLEAN unlocked_success;
} DETECTED_GPU;

#define MAX_SYSTEM_GPUS 8

/* Architecture Identification Helpers */
GPU_ARCH ClassifyGpuArchitecture(UINT16 device_id, CONST CHAR16 **chip_name, CONST CHAR16 **model_name);

#endif /* UNIVERSAL_GPU_TYPES_H */

