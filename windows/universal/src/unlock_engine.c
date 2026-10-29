/* SPDX-License-Identifier: GPL-2.0-only */
#include "unlock_engine.h"
#include "checks.h"
#include "compat/compat.h"
#include "logger.h"
#include "port.h"
#include "vendor/unlock_610.h"

static BOOLEAN UnlockDisplayOk(VOID) { return TRUE; }

EFI_STATUS ExecuteGpuRamUnlock(DETECTED_GPU *gpu) {
  if (!gpu || !gpu->pci_io) {
    LogWrite("[Unlock] Error: Invalid GPU handle or PCI IO\n");
    return EFI_INVALID_PARAMETER;
  }

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
  if (!EFI_ERROR(
          gpu->pci_io->Pci.Read(gpu->pci_io, EfiPciIoWidthUint32, 0, 64, c))) {
    GuCheckConfig(c, gpu->segment, gpu->bus, gpu->device, gpu->function, &caps);
  }
  Gu.pm_cap = caps.pm ? caps.pm : 0x60;
  Gu.pcie_cap = caps.pcie ? caps.pcie : 0x78;
  Gu.pcie_version = caps.pcie_version ? caps.pcie_version : 2;
  Gu.msi_cap = caps.msi;
  Gu.msix_cap = caps.msix;
  Gu.board_id = (gpu->device_id << 16) | gpu->vendor_id;
  Gu.board_subsystem = (gpu->subsys_device_id << 16) | gpu->subsys_vendor_id;
  Gu.owned = TRUE;
  Gu.armed = TRUE;
  Gu.display_ok = TRUE;
  Gu.display_check = UnlockDisplayOk;
  Gu.profile_verified = TRUE;
  Gu.prom_verified = TRUE;
  Gu.transaction_attempted = FALSE;

  /* Check and latch initial cold speed selectors */
  UINT32 cold_ss0 = GuRead32(0x82381c);
  UINT32 cold_ss1 = GuRead32(0x823820);
  LogWrite("[Unlock] Cold selectors before transaction: SS0=%#x SS1=%#x\n", cold_ss0, cold_ss1);

  if (cold_ss0 == 0x88888888U && (cold_ss1 & 0xFU) == 0x8U) {
    LogWrite("[Unlock] GPU is already unlocked in RAM (SS0=0x88888888, SS1=0x8)!\n");
    gpu->unlocked_success = TRUE;
    return EFI_SUCCESS;
  }

  if (!GuLatchEntrySelectors(cold_ss0, cold_ss1)) {
    LogWrite("[Unlock] Warning: GuLatchEntrySelectors returned FALSE (SS0=%#x, SS1=%#x)\n", cold_ss0, cold_ss1);
  }

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
  gpu->pci_io->Pci.Read(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &pci_cmd);
  pci_cmd = (pci_cmd & ~0x0004) | 0x0003;
  gpu->pci_io->Pci.Write(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &pci_cmd);
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
