#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Turn one schema-4 AD102 FE probe capture into host-test fixture headers.

The headers hold the UEFI ownership graph, the USB device paths on the pinned
keyboard/pointer host and the 256-byte PCI config space. They contain no ROM
or driver code. Handle IDs are the capture's enumeration indexes plus one, so
that index 0 is not a NULL handle. Endpoint IDs are derived with the same
rules msi_release.c applies at run time, not copied by hand.
"""
import argparse
import hashlib
import json
import struct
import uuid
from pathlib import Path

PROM = 'c03931779344e3b73002f6f8da2870327ac24242a6a8c76f9a42f11f1ca9f780'
PCI = '4cf5b200-68b8-4ca5-9eec-b23e3f50029a'
PATH = '09576e91-6d3f-11d2-8e39-00a0c969723b'
TEXT_OUT = '387477c2-69c7-11d2-8e39-00a0c969723b'
TEXT_IN = '387477c1-69c7-11d2-8e39-00a0c969723b'
GOP = '9042a9de-23dc-4a38-96fb-7aded080516a'
POINTER = '31878c87-0b75-11d5-9a4f-0090273fc14d'
USB_HC = '3e745226-9818-45b6-a2ac-d7cd0e8ba2bc'
USB_IO = '2b2f68d6-0cd2-44cf-8e8b-bba20b1b5b75'
# PciRoot(0)/Pci(8,1)/Pci(0,3): the xHCI that owns the console keyboard and pointer.
USB_HOST_PATH = bytes([2, 1, 12, 0, 0xd0, 0x41, 3, 10, 0, 0, 0, 0, 1, 1, 6, 0, 1, 8,
                       1, 1, 6, 0, 3, 0, 127, 255, 4, 0])
GPU_PATH = bytes([2, 1, 12, 0, 0xd0, 0x41, 3, 10, 0, 0, 0, 0, 1, 1, 6, 0, 1, 1,
                  1, 1, 6, 0, 0, 0, 127, 255, 4, 0])
BY_CHILD, BY_DRIVER = 8, 0x10


def guid_text(raw):
    return str(uuid.UUID(bytes_le=bytes.fromhex(raw)))


def guid_c(text):
    b = uuid.UUID(text).bytes_le
    d1, d2, d3 = struct.unpack('<IHH', b[:8])
    return '{%#x,%#x,%#x,{%s}}' % (d1, d2, d3, ','.join('%#x' % x for x in b[8:]))


def one(items, what):
    items = list(items)
    if len(items) != 1:
        raise SystemExit(f'{what}: expected exactly one, found {len(items)}')
    return items[0]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    p.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'tests')
    a = p.parse_args()
    if a.capture.stat().st_size > 16 * 1024 * 1024:
        p.error('Capture exceeds the bounded schema-4 input size')
    j = json.loads(a.capture.read_text(encoding='utf-8-sig'))
    if j.get('schema') != 4 or j.get('probe_target') != 'ad102-fe':
        p.error('Requires a schema-4 ad102-fe capture')
    if j['physical_prom'].get('sha256') != PROM:
        p.error('ROM differs from the reviewed profile; do not bypass admission')
    pl = j['platform']
    if not pl.get('complete'):
        p.error('Ownership graph is incomplete')
    handles = {h['id']: h for h in pl['handles']}
    path = {i: bytes.fromhex(h['path']) if h.get('path') else None for i, h in handles.items()}
    edges = []
    for h in pl['handles']:
        for o in h.get('active_opens', []):
            attrs = int(o['attributes'], 16)
            if attrs in (1, 2, 4):
                continue  # msi_release.c Snapshot() skips these.
            if o['agent'] is None or o['controller'] is None or not o['count']:
                p.error(f'Edge on handle {h["id"]} has no agent/controller')
            edges.append((h['id'], o['agent'], o['controller'], guid_text(o['protocol']), attrs, o['count']))

    gpu = pl['target']
    if path[gpu] != GPU_PATH:
        p.error('GPU path differs from msi_release.c PciPath')
    console = pl['conout']
    if pl['conin'] != console or pl['stderr'] != console:
        p.error('Console handles are split')
    nv = one((e for e in edges if e[0] == gpu and e[3] == PCI and e[4] == BY_DRIVER and e[2] == gpu), 'GPU PciIo owner')[1]
    gop = one((e for e in edges if e[0] == gpu and e[3] == PATH and e[4] == BY_CHILD), 'GOP child')[2]
    out = one((e for e in edges if e[0] == gop and e[3] == TEXT_OUT and e[4] == BY_DRIVER and e[2] == gop), 'TextOut owner')[1]
    gfx = one((e for e in edges if e[0] == gop and e[3] == GOP and e[4] == BY_DRIVER and e[2] == gop), 'GOP owner')[1]
    host_edge = one((e for e in edges if e[3] == USB_HC and e[4] == BY_DRIVER and e[0] == e[2]
                     and path[e[0]] == USB_HOST_PATH), 'pinned USB host')
    host, host_agent = host_edge[0], host_edge[1]
    prefix = USB_HOST_PATH[:-4]
    pointer_agent = one({e[1] for e in edges if e[3] == POINTER and e[2] == console and e[4] == BY_CHILD}, 'pointer splitter')
    key_agent = one({e[1] for e in edges if e[3] == TEXT_IN and e[2] == console and e[4] == BY_CHILD}, 'keyboard splitter')
    usb_io_agent = one({e[1] for e in edges if e[3] == USB_IO and path[e[0]] and path[e[0]].startswith(prefix)}, 'UsbIo agent')
    pointers = sorted({e[0] for e in edges if e[3] == POINTER and e[2] == console})
    agents = {}
    for g in pl['gpu_agents']:
        agents[g['agent']] = g
    if set(agents) != {nv, out, gfx} or agents[nv]['image_handle'] != nv:
        p.error('GPU agent list differs from the ownership graph')
    source = one({agents[out]['source_handle'], agents[gfx]['source_handle']}, 'console driver volume')
    used = set()
    for e in edges:
        used.update(e[:3])
    free = [i for i in sorted(handles) if i not in used and not path[i]]
    run = next(i for i in free if all(i + k in free for k in range(6)))
    singles = [i for i in reversed(free) if not run <= i < run + 6][:8]
    if len(singles) < 8:
        p.error('Not enough unused handles for simulated additions')

    def h(i):
        return f'(EFI_HANDLE){i + 1}'

    lines = ['/* AD102 FE CAP0001 ownership graph; handles are capture indexes plus one. */',
             '/* Generated by tools/make_test_fixtures.py. No ROM or driver code. */',
             f'#define CAP_HANDLES {len(handles)}', f'#define CAP_GPU {gpu}', f'#define CAP_GOP {gop}',
             f'#define CAP_CONSOLE {console}', f'#define CAP_OUT_AGENT {out}', f'#define CAP_GFX_AGENT {gfx}',
             f'#define CAP_NV_AGENT {nv}', f'#define CAP_OUT_IMAGE {agents[out]["image_handle"]}',
             f'#define CAP_GFX_IMAGE {agents[gfx]["image_handle"]}', f'#define CAP_DRIVER_VOLUME {source}',
             f'#define CAP_USB_HOST {host}', f'#define CAP_USB_AGENT {host_agent}', f'#define CAP_USB_IO_AGENT {usb_io_agent}',
             f'#define CAP_POINTER_AGENT {pointer_agent}', f'#define CAP_KEY_AGENT {key_agent}',
             f'#define CAP_POINTER_A {pointers[0]}', f'#define CAP_POINTER_B {pointers[-1]}',
             '/* Handles with no edge and no device path, used for simulated additions. */',
             f'#define CAP_FREE_RUN {run}']
    lines += [f'#define CAP_FREE_{k} {v}' for k, v in enumerate(singles)]
    lines.append('static const EDGE CapturedEdges[]={')
    for e in edges:
        lines.append(f'{{{h(e[0])},{h(e[1])},{h(e[2])},{guid_c(e[3])},{e[4]:#010x},{e[5]}}},')
    lines.append('};')
    (a.out / 'capture_graph.h').write_bytes(('\n'.join(lines) + '\n').encode())

    usb = sorted(i for i, b in path.items() if b and (b.startswith(prefix) or
                 any(e[0] == i and e[3] == USB_HC and e[4] == BY_DRIVER and e[0] == e[2] for e in edges)))
    lines = ['/* AD102 FE CAP0001 USB host and device paths; no firmware binary data. */',
             'static const struct {EFI_HANDLE handle;UINT8 path[128];} CapturedUsbPaths[]={']
    for i in usb:
        if len(path[i]) > 128:
            p.error('USB path exceeds fixture bound')
        lines.append(f'{{{h(i)},{{{",".join("%#x" % x for x in path[i])}}}}},')
    lines.append('};')
    (a.out / 'capture_usb_paths.h').write_bytes(('\n'.join(lines) + '\n').encode())

    cfg = bytes.fromhex(j['config256'])
    if len(cfg) != 256:
        p.error('Config space must be 256 bytes')
    text = ('/* PCI config captured on real hardware (AD102 FE CAP0001); other fake engine responses are simulations. */\n'
            'static const UINT8 CapturedConfig[256]={' + ','.join('%#04x' % x for x in cfg) + '};\n')
    (a.out / 'captured_config.h').write_bytes(text.encode())
    print(f'edges={len(edges)} usb_paths={len(usb)} gpu={gpu} gop={gop} console={console} '
          f'agents out={out} gfx={gfx} nv={nv} host={host}/{host_agent} keys={key_agent} pointer={pointer_agent} '
          f'pointers={pointers} free_run={run} singles={singles}')
    print('Wrote capture_graph.h, capture_usb_paths.h, captured_config.h; sha256 of capture '
          + hashlib.sha256(a.capture.read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
