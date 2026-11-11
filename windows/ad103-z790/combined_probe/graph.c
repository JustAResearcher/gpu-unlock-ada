/* SPDX-License-Identifier: GPL-2.0-only */
#include "graph.h"
#include <stddef.h>

int memcmp(const VOID *, const VOID *, size_t);

BOOLEAN GuGuidEqual(const EFI_GUID *a, const EFI_GUID *b) {
  return a && b && !memcmp(a, b, sizeof(EFI_GUID));
}

/* Same algorithm as msi_release.c's Snapshot(): for every handle, for every
 * protocol on it, for every agent with it open, record one edge -- except
 * the three query-only open modes (BY_HANDLE_PROTOCOL=1, GET_PROTOCOL=2,
 * TEST_PROTOCOL=4), which carry no real ownership and would just add noise.
 * Unlike Snapshot(), this never special-cases an edge as "our own" (this
 * tool never opens anything itself), and a structurally odd edge (no
 * controller, no agent, zero count) is logged as a separate counter rather
 * than treated as a fatal parse error -- the point here is to see what is
 * actually there, including anything unusual. */
EFI_STATUS GuSnapshotGraph(EFI_BOOT_SERVICES *Bs, GU_GRAPH *Out) {
  if (!Bs || !Out) return EFI_INVALID_PARAMETER;
  Out->count = 0;
  EFI_HANDLE *handles = NULL;
  UINTN handle_count = 0;
  EFI_STATUS s = Bs->LocateHandleBuffer(AllHandles, NULL, NULL, &handle_count, &handles);
  if (s == EFI_SUCCESS && (!handles || !handle_count || handle_count > 16384)) s = EFI_BAD_BUFFER_SIZE;
  for (UINTN i = 0; s == EFI_SUCCESS && i < handle_count; i++) {
    EFI_GUID **protocols = NULL;
    UINTN proto_count = 0;
    s = Bs->ProtocolsPerHandle(handles[i], &protocols, &proto_count);
    if (s == EFI_SUCCESS && (!protocols || !proto_count || proto_count > 128)) s = EFI_BAD_BUFFER_SIZE;
    for (UINTN j = 0; s == EFI_SUCCESS && j < proto_count; j++) {
      if (!protocols[j]) { s = EFI_COMPROMISED_DATA; break; }
      EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries = NULL;
      UINTN entry_count = 0;
      s = Bs->OpenProtocolInformation(handles[i], protocols[j], &entries, &entry_count);
      if (s == EFI_SUCCESS && (entry_count > 256 || (entry_count && !entries))) s = EFI_BAD_BUFFER_SIZE;
      for (UINTN k = 0; s == EFI_SUCCESS && k < entry_count; k++) {
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e = &entries[k];
        if (e->Attributes == 1 || e->Attributes == 2 || e->Attributes == 4) continue;
        if (Out->count == GU_GRAPH_MAX_EDGES) { s = EFI_OUT_OF_RESOURCES; break; }
        Out->edges[Out->count].handle = handles[i];
        Out->edges[Out->count].agent = e->AgentHandle;
        Out->edges[Out->count].controller = e->ControllerHandle;
        Out->edges[Out->count].protocol = *protocols[j];
        Out->edges[Out->count].attributes = e->Attributes;
        Out->edges[Out->count].count = e->OpenCount;
        Out->count++;
      }
      if (entries && Bs->FreePool(entries) != EFI_SUCCESS) s = EFI_DEVICE_ERROR;
    }
    if (protocols && Bs->FreePool(protocols) != EFI_SUCCESS) s = EFI_DEVICE_ERROR;
  }
  if (handles && Bs->FreePool(handles) != EFI_SUCCESS) s = EFI_DEVICE_ERROR;
  return s;
}

UINTN GuCountEdges(const GU_GRAPH *g, EFI_HANDLE Handle, const EFI_GUID *Protocol, UINT32 Attributes, EFI_HANDLE *AgentOut) {
  UINTN matches = 0;
  EFI_HANDLE agent = NULL;
  if (AgentOut) *AgentOut = NULL;
  if (!g) return 0;
  for (UINTN i = 0; i < g->count; i++) {
    const GU_EDGE *e = &g->edges[i];
    if (e->handle == Handle && e->attributes == Attributes && GuGuidEqual(&e->protocol, Protocol)) {
      agent = e->agent;
      matches++;
    }
  }
  if (matches == 1 && AgentOut) *AgentOut = agent;
  return matches;
}
