/* SPDX-License-Identifier: GPL-2.0-only */
/* Universal UEFI console detach and ownership claim for AD104 RTX 4070 Ti.
 * Adapted from impacifist/Windows-GPU-UNLOCK internal-boot/unlock/ownership.c.
 * Dynamic UEFI handle/binding discovery; supports AMI Aptio and other standard UEFI BIOSes.
 */
#include "x99_release.h"
#include "boot_io.h"
#include "profile.h"
#include "compat/compat.h"
#include <Protocol/GraphicsOutput.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>

static struct {
  EFI_SYSTEM_TABLE *table;
  EFI_HANDLE self, pci, nv_agent;
  EFI_PCI_IO_PROTOCOL *io;
  UINT32 config[64];
  UINT64 attributes;
  BOOLEAN ready, mutated, claimed;
} ReleasePlan;

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;

static EFI_STATUS FindOwner(VOID) {
  EFI_BOOT_SERVICES *bs = ReleasePlan.table->BootServices;
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries = NULL;
  UINTN count = 0;
  EFI_STATUS s = bs->OpenProtocolInformation(ReleasePlan.pci, &PciGuid, &entries, &count);
  if (EFI_ERROR(s)) return s;
  ReleasePlan.nv_agent = NULL;
  for (UINTN i = 0; i < count; i++) {
    if (entries[i].Attributes == EFI_OPEN_PROTOCOL_BY_DRIVER && entries[i].ControllerHandle == ReleasePlan.pci) {
      ReleasePlan.nv_agent = entries[i].AgentHandle;
      break;
    }
  }
  if (entries) bs->FreePool(entries);
  return ReleasePlan.nv_agent ? EFI_SUCCESS : EFI_NOT_FOUND;
}

EFI_STATUS GuX99Preflight(EFI_SYSTEM_TABLE *t, EFI_HANDLE self, EFI_HANDLE pci) {
  if (ReleasePlan.mutated || ReleasePlan.ready) return EFI_ALREADY_STARTED;
  memset(&ReleasePlan, 0, sizeof(ReleasePlan));
  ReleasePlan.table = t; ReleasePlan.self = self; ReleasePlan.pci = pci;
  if (!t || !self || !pci || Gu.handle != pci || !Gu.profile_verified || !Gu.prom_verified || Gu.started)
    return EFI_INVALID_PARAMETER;
  if (!GuAd104ColdEntry()) return EFI_NOT_READY;
  EFI_STATUS s = FindOwner();
  if (EFI_ERROR(s)) {
    GuLog("Release: GOP driver owner not found: %#llx\n", (unsigned long long)s);
    return s;
  }
  for (UINTN i = 0; i < 64; i++) {
    s = GuConfigRead(EfiPciIoWidthUint32, (UINT32)i * 4, &ReleasePlan.config[i]);
    if (EFI_ERROR(s)) return s;
  }
  if (Gu.pci->Attributes) {
    s = Gu.pci->Attributes(Gu.pci, EfiPciIoAttributeOperationGet, 0, &ReleasePlan.attributes);
    if (EFI_ERROR(s)) return s;
  }
  GuLog("X99 Preflight: GPU handle=%#llx GOP owner agent=%#llx\n",
    (unsigned long long)(UINTN)pci, (unsigned long long)(UINTN)ReleasePlan.nv_agent);
  ReleasePlan.ready = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS GuX99Release(VOID) {
  if (!ReleasePlan.ready || ReleasePlan.mutated || Gu.started || Gu.owned) return EFI_NOT_READY;
  EFI_BOOT_SERVICES *bs = ReleasePlan.table->BootServices;
  GuLog("X99 driver detach intent: agent=%#llx\n", (unsigned long long)(UINTN)ReleasePlan.nv_agent);
  ReleasePlan.mutated = TRUE;
  EFI_STATUS s = bs->DisconnectController(ReleasePlan.pci, ReleasePlan.nv_agent, NULL);
  GuLog("X99 driver detach return: status=%#llx\n", (unsigned long long)s);
  if (EFI_ERROR(s)) return s;

  /* Claim exclusive ownership */
  EFI_PCI_IO_PROTOCOL *io = NULL;
  s = bs->OpenProtocol(ReleasePlan.pci, &PciGuid, (VOID **)&io, ReleasePlan.self, NULL, EFI_OPEN_PROTOCOL_EXCLUSIVE);
  GuLog("X99 exclusive claim return: status=%#llx\n", (unsigned long long)s);
  if (EFI_ERROR(s)) return s;
  ReleasePlan.claimed = TRUE;
  Gu.owned = TRUE;
  ReleasePlan.io = io;

  /* Restore Memory & IO decode bits if cleared by DisconnectController */
  UINT32 current[64];
  for (UINTN i = 0; i < 64; i++) GuConfigRead(EfiPciIoWidthUint32, (UINT32)i * 4, &current[i]);
  UINT32 missing = ReleasePlan.config[1] & ~current[1] & 3;
  if (missing) {
    UINT64 mask = (missing & 1 ? EFI_PCI_IO_ATTRIBUTE_IO : 0) | (missing & 2 ? EFI_PCI_IO_ATTRIBUTE_MEMORY : 0);
    s = io->Attributes(io, EfiPciIoAttributeOperationEnable, mask, NULL);
    GuLog("X99 decode restore: mask=%#llx status=%#llx\n", (unsigned long long)mask, (unsigned long long)s);
    if (EFI_ERROR(s)) return s;
  }
  if (!GuAd104ColdEntry()) {
    GuLog("X99 cold check failed after release\n");
    return EFI_NOT_READY;
  }
  return EFI_SUCCESS;
}

BOOLEAN GuX99Released(VOID) {
  return ReleasePlan.ready && ReleasePlan.mutated && ReleasePlan.claimed && Gu.owned && !Gu.fault;
}

EFI_STATUS GuX99Finish(VOID) {
  if (!GuSafeToReturn() || !Gu.quiesced || Gu.flrs != 1 || !GuX99Released()) return EFI_NOT_READY;
  EFI_STATUS s = ReleasePlan.table->BootServices->CloseProtocol(ReleasePlan.pci, &PciGuid, ReleasePlan.self, NULL);
  if (s == EFI_SUCCESS) {
    ReleasePlan.claimed = FALSE;
    Gu.owned = FALSE;
  }
  GuLog("X99 finish close status=%#llx\n", (unsigned long long)s);
  return s;
}

BOOLEAN GuX99ReadyForWindows(VOID) {
  if (!ReleasePlan.ready || ReleasePlan.claimed || Gu.owned || Gu.fault || !Gu.quiesced || Gu.flrs != 1 || !GuSafeToReturn())
    return FALSE;
  UINT32 chip = GuRead32(0), cpu = GuRead32(0x110100), hi = GuRead32(0x1fa828);
  UINT32 ss0 = GuRead32(0x82381c), ss1 = GuRead32(0x823820), readout = GuRead32(0x823818), plm = GuRead32(0x823804);
  return !Gu.fault && chip == 0x194000a1 && (cpu & 0x10) && cpu != 0xffffffff &&
    (cpu & 0xffff0000) != 0xbadf0000 && !(hi >> 4) && ss0 == 0x88888888 && ss1 == 8 &&
    !(readout & 0x07ffffff) && readout != 0xffffffff && plm == 0xffffffff;
}

BOOLEAN GuX99Mutated(VOID) {
  return ReleasePlan.mutated;
}

