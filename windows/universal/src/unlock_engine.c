/* SPDX-License-Identifier: GPL-2.0-only */
#include "unlock_engine.h"
#include "checks.h"
#include "compat/compat.h"
#include "logger.h"
#include "port.h"
#include "vendor/unlock_610.h"
#include "vendor/vbios_610.h"

static DETECTED_GPU *g_current_unlock_gpu = NULL;

static BOOLEAN CheckGpuDisplayAndOwnershipOk(VOID) {
  if (!g_current_unlock_gpu || !g_current_unlock_gpu->pci_io) return FALSE;
  UINT16 cmd = 0;
  EFI_STATUS s = g_current_unlock_gpu->pci_io->Pci.Read(
      g_current_unlock_gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &cmd);
  if (EFI_ERROR(s)) return FALSE;
  /* Memory decode must be active, Bus Master must be disabled */
  if ((cmd & 0x0002) == 0 || (cmd & 0x0004) != 0) return FALSE;
  return TRUE;
}

static BOOLEAN ValidateVbiosEvidence(DETECTED_GPU *gpu) {
  if (!gpu || !gpu->vbios_loaded || !gpu->vbios_data || gpu->vbios_size < 0x200) {
    LogWrite("[Unlock] VBIOS validation failed: missing or truncated image\n");
    return FALSE;
  }

  const UINT8 *rom = (const UINT8 *)gpu->vbios_data;
  /* 1. Validate Option ROM signature: 0x55, 0xAA */
  if (rom[0] != 0x55 || rom[1] != 0xAA) {
    LogWrite("[Unlock] VBIOS validation failed: missing 0x55 0xAA signature\n");
    return FALSE;
  }

  /* 2. Validate PCIR structure */
  UINT16 pcir_offset = (UINT16)rom[0x18] | ((UINT16)rom[0x19] << 8);
  if ((UINTN)pcir_offset + 0x20 > gpu->vbios_size) {
    LogWrite("[Unlock] VBIOS validation failed: invalid PCIR offset %#x\n", pcir_offset);
    return FALSE;
  }

  const UINT8 *pcir = rom + pcir_offset;
  if (pcir[0] != 'P' || pcir[1] != 'C' || pcir[2] != 'I' || pcir[3] != 'R') {
    LogWrite("[Unlock] VBIOS validation failed: PCIR signature missing\n");
    return FALSE;
  }

  UINT16 vendor_id = (UINT16)pcir[4] | ((UINT16)pcir[5] << 8);
  UINT16 device_id = (UINT16)pcir[6] | ((UINT16)pcir[7] << 8);
  if (vendor_id != 0x10de || device_id != gpu->device_id) {
    LogWrite("[Unlock] VBIOS validation failed: ID mismatch (ROM: %#x:%#x, GPU: %#x:%#x)\n",
             vendor_id, device_id, gpu->vendor_id, gpu->device_id);
    return FALSE;
  }

  /* 3. Validate NVIDIA FWSEC structure using parser */
  struct gpu_unlock_fwsec fw = {0};
  int fw_err = gpu_unlock_vbios_fwsec(rom, gpu->vbios_size, &fw);
  if (fw_err != 0) {
    LogWrite("[Unlock] VBIOS validation failed: FWSEC parsing returned error %d\n", fw_err);
    return FALSE;
  }

  LogWrite("[Unlock] VBIOS evidence verified: Vendor=%#x Dev=%#x FWSEC ucode=%#x sig_ver=%#x sig_bytes=%u\n",
           vendor_id, device_id, (unsigned)fw.ucode_id, (unsigned)fw.sig_versions, (unsigned)fw.sig_bytes);
  return TRUE;
}

EFI_STATUS ExecuteGpuRamUnlock(DETECTED_GPU *gpu) {
  if (!gpu || !gpu->pci_io) {
    LogWrite("[Unlock] Error: Invalid GPU handle or PCI IO\n");
    return EFI_INVALID_PARAMETER;
  }

  g_current_unlock_gpu = gpu;

  LogWrite("[Unlock] Initializing Falcon/GSP V67 transaction for Bus %u:%u.%u "
           "(DevID=%#x SubVen=%#x)\n",
           (unsigned)gpu->bus, (unsigned)gpu->device, (unsigned)gpu->function,
           gpu->device_id, gpu->subsys_vendor_id);

  /* 1. Setup GPU platform state in Gu */
  GuInit(gBS, gpu->pci_io);
  Gu.image = gImageHandle;
  Gu.handle = gpu->handle;
  Gu.bar_base = gpu->bar0_base;
  Gu.bar_size = gpu->bar0_size ? gpu->bar0_size : 0x1000000;

  UINT32 c[64] = {0};
  GU_CAPS caps = {0};
  EFI_STATUS pci_st = gpu->pci_io->Pci.Read(gpu->pci_io, EfiPciIoWidthUint32, 0, 64, c);
  if (EFI_ERROR(pci_st)) {
    LogWrite("[Unlock] Error: Failed to read PCI config space (%#llx)\n", (unsigned long long)pci_st);
    return pci_st;
  }

  if (!GuCheckConfig(c, gpu->segment, gpu->bus, gpu->device, gpu->function, &caps)) {
    LogWrite("[Unlock] Error: GuCheckConfig rejected PCI configuration for GPU on Bus %u:%u.%u\n",
             (unsigned)gpu->bus, (unsigned)gpu->device, (unsigned)gpu->function);
    return EFI_SECURITY_VIOLATION;
  }

  Gu.pm_cap = caps.pm ? caps.pm : 0x60;
  Gu.pcie_cap = caps.pcie ? caps.pcie : 0x78;
  Gu.pcie_version = caps.pcie_version ? caps.pcie_version : 2;
  Gu.msi_cap = caps.msi;
  Gu.msix_cap = caps.msix;
  Gu.board_id = (gpu->device_id << 16) | gpu->vendor_id;
  Gu.board_subsystem = (gpu->subsys_device_id << 16) | gpu->subsys_vendor_id;

  /* 1. Validate VBIOS evidence BEFORE latching selectors */
  if (!ValidateVbiosEvidence(gpu)) {
    LogWrite("[Unlock] Error: VBIOS evidence validation failed for GPU on Bus %u:%u.%u\n",
             (unsigned)gpu->bus, (unsigned)gpu->device, (unsigned)gpu->function);
    return EFI_COMPROMISED_DATA;
  }
  Gu.prom_verified = TRUE;

  /* 2. Check and latch initial cold speed selectors (requires prom_verified == TRUE) */
  UINT32 cold_ss0 = GuRead32(0x82381c);
  UINT32 cold_ss1 = GuRead32(0x823820);
  LogWrite("[Unlock] Cold selectors before transaction: SS0=%#x SS1=%#x\n", cold_ss0, cold_ss1);

  if (cold_ss0 == 0x88888888U && (cold_ss1 & 0xFU) == 0x8U) {
    LogWrite("[Unlock] GPU is already unlocked in RAM (SS0=0x88888888, SS1=0x8)!\n");
    gpu->unlocked_success = TRUE;
    return EFI_SUCCESS;
  }

  if (!GuLatchEntrySelectors(cold_ss0, cold_ss1)) {
    LogWrite("[Unlock] Error: GuLatchEntrySelectors failed to latch valid entry selectors (SS0=%#x, SS1=%#x)\n",
             cold_ss0, cold_ss1);
    return EFI_DEVICE_ERROR;
  }

  /* 3. Mark profile, ownership and display check */
  Gu.profile_verified = TRUE;
  Gu.owned = TRUE;
  Gu.armed = TRUE;
  Gu.display_ok = TRUE;
  Gu.display_check = CheckGpuDisplayAndOwnershipOk;
  Gu.transaction_attempted = FALSE;

  /* 2. Setup Linux pci_dev structure */
  UINT64 mask = 0xffffffffffffULL;
  struct pci_dev dev = {
      .vendor = gpu->vendor_id,
      .device = gpu->device_id,
      .dev = {.dma_mask = &mask, .coherent_dma_mask = 0xfffffffffULL},
      .current_state = PCI_D0,
      .pm_cap = Gu.pm_cap,
      .refs = 1};

  /* Ensure IO (0x1) and Memory (0x2) are enabled, and Bus Master (0x4) is 0 before transaction claim */
  UINT16 pci_cmd = 0;
  pci_st = gpu->pci_io->Pci.Read(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &pci_cmd);
  if (EFI_ERROR(pci_st)) {
    LogWrite("[Unlock] Error: Failed to read PCI command register: %#llx\n", (unsigned long long)pci_st);
    return pci_st;
  }
  pci_cmd = (pci_cmd & ~0x0004) | 0x0003;
  pci_st = gpu->pci_io->Pci.Write(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &pci_cmd);
  if (EFI_ERROR(pci_st)) {
    LogWrite("[Unlock] Error: Failed to write PCI command register: %#llx\n", (unsigned long long)pci_st);
    return pci_st;
  }
  LogWrite("[Unlock] PCI command prepared: %#x (IO/Memory=ENABLED, Bus Master=DISABLED)\n", pci_cmd);

  /* 3. Run V67 firmware transaction (FWSEC -> SEC2 Booter -> Selectors -> FLR)
   */
  struct gpu_unlock_610_shared shared = {0};
  int load_res = gpu_unlock_610_load_shared(&dev, &shared);
  if (load_res != 0) {
    LogWrite("[Unlock] Error: gpu_unlock_610_load_shared failed: %d\n",
             load_res);
    return EFI_DEVICE_ERROR;
  }

  int unlock_res = gpu_unlock_610_unlock(&dev, &shared);
  gpu_unlock_610_release_shared(&shared);

  LogWrite("Transaction result=%d safe=%u retained=%u started=%u quiesced=%u "
           "flrs=%u DMA_live=%u\n",
           unlock_res, GuSafeToReturn(), Gu.retained, Gu.started, Gu.quiesced,
           Gu.flrs, Gu.dma_count);

  if (unlock_res != 0 || Gu.fault || !GuSafeToReturn()) {
    LogWrite("[Unlock] FAILED: Falcon transaction error=%d fault=%u safe=%u\n",
             unlock_res, Gu.fault, GuSafeToReturn());
    return EFI_DEVICE_ERROR;
  }

  /* 4. Verify post-FLR readback of latched speed selectors */
  UINT32 ss0 = GuRead32(0x82381c);
  UINT32 ss1 = GuRead32(0x823820);
  LogWrite("[Unlock] Readback post-FLR: SS0=%#x SS1=%#x\n", ss0, ss1);

  if (ss0 == 0x88888888U && (ss1 & 0xFU) == 0x8U) {
    gpu->unlocked_success = TRUE;
    LogWrite("[Unlock] SUCCESS: Selectors permanently latched in RAM "
             "(SS0=0x88888888, SS1=0x8)!\n");
    return EFI_SUCCESS;
  }

  LogWrite("[Unlock] FAILED: Selectors did not latch (read SS0=%#x, SS1=%#x)\n",
           ss0, ss1);
  return EFI_DEVICE_ERROR;
}
