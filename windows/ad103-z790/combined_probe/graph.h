/* SPDX-License-Identifier: GPL-2.0-only */
/* Read-only protocol-ownership graph snapshot and lookup -- the discovery
 * counterpart of windows/ad104-msi/msi_release.c's Snapshot()/Consumers():
 * same structure and the same Attributes-1/2/4 filter (those are query-only
 * opens, not real ownership edges), but this never compares against a
 * baseline and never mutates anything. It only walks LocateHandleBuffer,
 * ProtocolsPerHandle and OpenProtocolInformation -- none of which open,
 * close, connect or disconnect anything. */
#pragma once
#include <Uefi.h>

#define GU_GRAPH_MAX_EDGES 4096U

typedef struct {
  EFI_HANDLE handle, agent, controller;
  EFI_GUID protocol;
  UINT32 attributes, count;
} GU_EDGE;

typedef struct {
  UINTN count;
  GU_EDGE edges[GU_GRAPH_MAX_EDGES];
} GU_GRAPH;

BOOLEAN GuGuidEqual(const EFI_GUID *a, const EFI_GUID *b);

/* Bs must be the boot services table. Fails closed: on any firmware error,
 * or if the live graph is larger than GU_GRAPH_MAX_EDGES, returns an error
 * and Out->count reflects whatever was captured before the failure. */
EFI_STATUS GuSnapshotGraph(EFI_BOOT_SERVICES *Bs, GU_GRAPH *Out);

/* Counts edges in g with the given Handle/Protocol/Attributes. If exactly
 * one matches, *AgentOut is set to its agent handle; otherwise *AgentOut is
 * set to NULL (ambiguous or absent is itself a result worth reporting, not
 * a fatal error here -- this is a capture tool, not an enforcement gate). */
UINTN GuCountEdges(const GU_GRAPH *g, EFI_HANDLE Handle, const EFI_GUID *Protocol, UINT32 Attributes, EFI_HANDLE *AgentOut);
