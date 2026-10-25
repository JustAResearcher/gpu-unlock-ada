#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Bind a completed rehearsal and a separate user-observed Windows boot.

The report is unauthenticated evidence of a prior no-unlock rehearsal, not
permission to execute a payload and not evidence that an unlock has worked.
An explicit build mode and runtime arming remain separate requirements.
"""
from __future__ import annotations

import copy
import hashlib
import re

from board_profile import HEX
from rehearsal_profile import (Reader, digest, lines, number, rehearsal_header,
                               require, validate_restoration_evidence)

FINAL = ('FINAL mode=firmware-rehearsal status=0 stage=windows-handoff mutated=1 '
         'unlock_attempted=0 app_mmio_writes=0 windows_handoff=pending')
SOURCE_VERSION = '0.5.2-rehearsal'
N = r'(0x[0-9a-f]+|[0-9]+)'


def validate_staged_evidence(profile: dict) -> dict:
    """Check serialized provenance and scope before header regeneration."""
    prior = validate_restoration_evidence(profile)
    require(profile.get('unlock_authorized') is False
            and profile.get('rehearsal_hardware_tested') is False
            and profile.get('purpose') == 'firmware-release-restoration-rehearsal',
            'staged evidence cannot change profile authorization')
    e = profile.get('staged_unlock_evidence')
    keys = {'schema_version', 'source_version', 'report_sha256', 'profile_tag',
            'topology_sha256', 'restoration_report_sha256', 'baseline_edges',
            'stage_edges', 'deferred_observations', 'restoration_verified',
            'unlock_attempted', 'app_mmio_writes', 'windows_handoff',
            'windows_boot_user_confirmed', 'windows_preload'}
    require(isinstance(e, dict) and set(e) == keys, 'missing or malformed staged evidence')
    require(type(e['schema_version']) is int and e['schema_version'] == 1
            and e['source_version'] == SOURCE_VERSION
            and e['restoration_verified'] is True and e['unlock_attempted'] is False
            and type(e['app_mmio_writes']) is int and e['app_mmio_writes'] == 0
            and e['windows_handoff'] == 'pending'
            and e['windows_boot_user_confirmed'] is True,
            'invalid staged evidence scope')
    for key in ('report_sha256','profile_tag','topology_sha256','restoration_report_sha256'):
        require(isinstance(e[key], str), 'invalid staged evidence digest')
        digest(e[key])
    require(e['profile_tag'] == profile['board']['report_sha256']
            and e['topology_sha256'] == profile['topology_sha256']
            and e['restoration_report_sha256'] == prior['report_sha256'],
            'staged evidence provenance mismatch')
    edges = e['baseline_edges']
    require(type(edges) is int and 6 <= edges <= 4096
            and type(e['deferred_observations']) is int and e['deferred_observations'] == 3
            and isinstance(e['stage_edges'], list)
            and all(type(v) is int for v in e['stage_edges'])
            and e['stage_edges'] == [edges-2,edges-3,edges-6,edges-3,edges-2,edges],
            'invalid verified restoration stage sequence')
    preload = e['windows_preload']
    require(isinstance(preload, dict) and set(preload) == {'bytes','sha256'}
            and type(preload['bytes']) is int and 512 <= preload['bytes'] <= 32*1024*1024
            and isinstance(preload['sha256'], str), 'invalid Windows preload evidence')
    digest(preload['sha256'])
    return e


def bind_rehearsal_success(profile: dict, data: bytes, windows_boot_confirmed: bool) -> dict:
    """Parse the whole ordered 0.5.2 success report and bind observed boot.

    The final report only says handoff is pending. A strict True argument is a
    separate user observation; it must never be inferred from the log itself.
    """
    require(type(windows_boot_confirmed) is bool and windows_boot_confirmed,
            'Windows boot must be confirmed separately by the user')
    prior = validate_restoration_evidence(profile)
    require('staged_unlock_evidence' not in profile, 'staged evidence already bound')
    rows = lines(data, 131072)
    require(rows[-1] == FINAL and sum(row.startswith('FINAL ') for row in rows) == 1,
            'requires a unique terminal handoff-pending report')
    r = Reader(data)
    board = profile['board']

    def exact(text):
        require(r.position < len(r.lines) and r.lines[r.position] == text,
                'unexpected staged evidence record; expected '+text)
        r.position += 1

    def regex(pattern):
        require(r.position < len(r.lines), 'truncated staged evidence')
        match = re.fullmatch(pattern,r.lines[r.position])
        require(match is not None, 'unexpected staged evidence line')
        r.position += 1
        return match

    def numeric(pattern):
        return [number(v) for v in regex(pattern).groups()]

    def check(stage): exact('CHECK stage='+stage+' status=0')

    def fields(keys):
        values = r.record('PROFILE',keys)
        for key,value in values.items():
            if key.endswith('sha256'):
                value = digest(value)
            else:
                pattern = '[0-9a-f]+' if key in HEX else '[0-9]+'
                require(re.fullmatch(pattern,value) is not None and len(value) <= 16,
                        'invalid staged board field')
                value = int(value,16 if key in HEX else 10)
            require(value == board[key], 'staged board differs from capture')

    def bdf():
        prefix = (f'target BDF={board["segment"]:x}:{board["bus"]:x}:'
                  f'{board["device"]:x}.{board["function"]:x} checks=1')
        values = numeric(re.escape(prefix)+r' pm='+N+r' pcie='+N+r' msi='+N+r' msix='+N)
        require(all(v < 256 for v in values), 'invalid PCI capability offset')

    def location():
        exact('target location: status=0'); bdf()

    def bar():
        exact('bar0 query: status=0')
        values = numeric(r'bar0 descriptor: type=0 granularity=32 min='+N+r' max='+N+
                         r' length='+N+r' translation=0')
        require(values[0] == board['bar_base'] and values[2] == board['bar_size']
                and values[1] in (board['bar_base']+board['bar_size']-1,board['bar_size']-1),
                'staged BAR differs from capture')
        fields('bar_base bar_size')

    def cold():
        fields('chip vram_mib')
        values = numeric(r'cold state: chip='+N+r' FEAT='+N+r' GFW='+N+'/'+N+'/'+N+
                         r' WPR2_HI=0 SS0='+N+r' SS1='+N)
        require(values[0] == board['chip'] and all(v < 2**32 for v in values),
                'invalid cold-state evidence')
        return values

    exact('gpu-unlock-ada Windows preboot port '+SOURCE_VERSION)
    regex(r'RUN[0-9]{2}')
    exact('PROFILE_TAG='+board['report_sha256']+' mode=firmware-rehearsal')
    exact('REHEARSAL_ONLY: active firmware driver callbacks; no unlock payload, direct GPU writes, DMA, FLR, flashing or persistent boot changes.')
    exact('SecureBoot query: status=0 bytes=1 value=0'); check('secure-boot')
    handles = numeric(r'PCI enumeration: status=0 handles='+N)[0]
    require(0 < handles <= 4096, 'invalid PCI enumeration size')
    exact('PCI target matches=1 status=0'); exact('target location: status=0')
    config = []
    for offset in range(0,256,16):
        match = regex(f'config\\[{offset:02x}\\]='+' '.join(['([0-9a-f]{8})']*4))
        config.extend(int(v,16) for v in match.groups())
    require(config[0] == board['id'] and config[11] == board['subsystem']
            and config[2] == 0x030000a1 and config[4] == board['bar_base']
            and config[1]&7 == 3, 'staged PCI configuration mismatch')
    bdf()
    fields('version id subsystem segment bus device function'); check('target')
    bar(); check('bar0')
    initial_cold = cold(); check('cold-state')
    rom_entries, previous_rom_offset, last_rom = 0, -1, 0
    while r.position < len(r.lines) and r.lines[r.position].startswith('gpu_unlock: ROM chain:'):
        values = numeric(r'gpu_unlock: ROM chain: pos='+N+r' data='+N+r' bytes='+N+r' type='+N+r' last='+N)
        rom_entries += 1
        require(rom_entries <= 16 and previous_rom_offset < values[0] < board['prom_size']
                and (rom_entries != 1 or values[0] == 0) and last_rom == 0
                and 0 < values[2] <= board['prom_size']-values[0] and values[4] in (0,128)
                and all(v < 2**32 for v in values), 'invalid ROM chain evidence')
        previous_rom_offset, last_rom = values[0], values[4]
    require(rom_entries > 0 and last_rom == 128, 'missing or incomplete ROM chain evidence')
    patterns = [
        r'gpu_unlock: ROM expansion: base='+N+r' ext='+N+r' adjust='+N,
        r'gpu_unlock: ROM BIT: offset='+N+r' size='+N+r' stride='+N+r' tokens='+N+r' adjust='+N,
        r'gpu_unlock: ROM ucode table: offset='+N+r' version='+N+r' header='+N+r' stride='+N+r' entries='+N,
        r'gpu_unlock: FWSEC descriptor: offset='+N+r' ROM_bytes='+N+r' header='+N,
        r'gpu_unlock: FWSEC geometry: image_off='+N+r' size='+N+r' imem='+N+r' va='+N+r' pa='+N+
        r' dmem='+N+r' pa='+N+r' pkc='+N+r' interface='+N+r' engine='+N+r' ucode='+N+
        r' signatures='+N+r' versions='+N+r' bytes='+N,
    ]
    for pattern in patterns:
        require(all(v < 2**32 for v in numeric(pattern)), 'oversized ROM diagnostic value')
    fields('prom_size prom_sha256'); fields('rom_size rom_sha256'); fields('fwsec_parsed'); check('prom')
    exact('GOP physical=1 target=1 expect_detached=0 status=0'); check('display')
    preload = r.record('WINDOWS_PRELOAD','internal_gpt_esp candidates bytes sha256 same_boot_pin')
    require(preload['internal_gpt_esp'] == preload['candidates'] == preload['same_boot_pin'] == '1',
            'ambiguous or unpinned Windows preload')
    preload = {'bytes':number(preload['bytes']), 'sha256':digest(preload['sha256'])}
    require(512 <= preload['bytes'] <= 32*1024*1024, 'Windows preload size outside bounds')
    check('windows-preload')
    observation = r.record('REHEARSAL_GOP_OBSERVATION','stage reason handle expected original console interface readable')
    require(observation.pop('reason') == 'baseline', 'not a baseline GOP observation')
    observation = {key:number(value) for key,value in observation.items()}
    require(observation['stage'] == 0 and observation['readable'] == 1
            and all(observation[key] for key in ('handle','expected','original','console','interface'))
            and observation['handle'] == observation['expected'] == observation['original']
            and observation['handle'] != observation['console'], 'invalid baseline GOP identity')
    callbacks = r.ints('REHEARSAL_GOP_CALLBACKS','stage query set blt mode_ptr mode_readable')
    require(callbacks['stage'] == 0 and callbacks['mode_readable'] == 1
            and all(callbacks[k] for k in ('query','set','blt','mode_ptr')), 'invalid GOP callback evidence')
    mode = r.ints('REHEARSAL_GOP_MODE','stage max mode info_ptr info_bytes info_readable framebuffer framebuffer_bytes')
    require(mode['stage'] == 0 and mode['info_ptr'] and mode['info_readable'] == 1, 'unreadable GOP mode')
    info = r.record('REHEARSAL_GOP_INFO','stage version width height format stride masks')
    masks = info.pop('masks').split('/')
    require(len(masks) == 4, 'invalid GOP pixel masks')
    info = {key:number(value,2**32-1) for key,value in info.items()}
    require(info.pop('stage') == 0, 'invalid GOP information stage')
    info['pixel_format'] = info.pop('format'); info['masks'] = [number(v,2**32-1) for v in masks]
    cleaned = dict(max_mode=mode['max'],mode=mode['mode'],info_bytes=mode['info_bytes'],
                   framebuffer_base=mode['framebuffer'],framebuffer_bytes=mode['framebuffer_bytes'],info=info)
    require(cleaned == prior['baseline'], 'successful rehearsal display differs from the bound baseline')
    prepared = r.ints('REHEARSAL_PREPARE','status edges drivers outputs')
    edges = prepared['edges']
    require(prepared['status'] == 0 and 6 <= edges <= 4096 and prepared['drivers'] == 3
            and prepared['outputs'] == 2, 'invalid rehearsal preparation')
    check('rehearsal-preflight')
    for text in ('READY: firmware release/restoration rehearsal; unlock payload is absent.',
                 'Press R to release three boot-display drivers, reconnect them, then start Windows only after verification.',
                 'This active firmware test may blank or hang the display. If Windows has not appeared after 2 minutes, power OFF, remove USB and restore security settings.',
                 'Esc or 120 seconds cancels before release. No automatic retry.'):
        exact(text)
    check('arming'); check('watchdog'); location(); check('armed-config')
    require(cold() == initial_cold, 'armed cold state changed'); check('armed-cold')
    exact('REHEARSAL_INTENT: R received; beginning staged release/restoration.')
    for stage,removed in ((1,2),(2,3),(3,6)):
        exact(f'REHEARSAL_INTENT stage={stage} operation=disconnect')
        exact(f'REHEARSAL_RETURN stage={stage} status=0')
        exact(f'REHEARSAL_STAGE_VERIFIED stage={stage} edges={edges-removed}')
    def deferred():
        d = prior['deferred']
        exact(f'REHEARSAL_GOP_DEFERRED stage=4 accepted=1 mode={d["mode"]} max={d["max_mode"]} framebuffer=0 framebuffer_bytes=0')
    exact('REHEARSAL_INTENT stage=4 operation=connect')
    exact('REHEARSAL_SUPPORTED stage=4 status=0'); exact('REHEARSAL_RETURN stage=4 status=0')
    deferred(); exact(f'REHEARSAL_STAGE_VERIFIED stage=4 edges={edges-3}')
    exact('REHEARSAL_INTENT stage=5 operation=connect'); deferred()
    exact('REHEARSAL_SUPPORTED stage=5 status=0'); deferred()
    exact('REHEARSAL_RETURN stage=5 status=0'); exact(f'REHEARSAL_STAGE_VERIFIED stage=5 edges={edges-2}')
    exact('REHEARSAL_INTENT stage=6 operation=connect')
    exact('REHEARSAL_SUPPORTED stage=6 status=0'); exact('REHEARSAL_RETURN stage=6 status=0')
    exact(f'REHEARSAL_STAGE_VERIFIED stage=6 edges={edges}')
    location(); check('restored-config'); bar(); check('restored-bar0')
    require(cold() == initial_cold, 'restored cold state changed'); check('restored-cold-state')
    exact('REHEARSAL_RESTORED: display ownership and PCI/cold state verified.')
    exact(FINAL)
    require(r.position == len(r.lines), 'extra staged evidence records')
    result = copy.deepcopy(profile)
    result['staged_unlock_evidence'] = dict(schema_version=1,source_version=SOURCE_VERSION,
        report_sha256=hashlib.sha256(data).hexdigest(),profile_tag=board['report_sha256'],
        topology_sha256=profile['topology_sha256'],restoration_report_sha256=prior['report_sha256'],
        baseline_edges=edges,stage_edges=[edges-2,edges-3,edges-6,edges-3,edges-2,edges],
        deferred_observations=3,restoration_verified=True,unlock_attempted=False,app_mmio_writes=0,
        windows_handoff='pending',windows_boot_user_confirmed=True,windows_preload=preload)
    validate_staged_evidence(result)
    return result


def staged_header(profile: dict) -> str:
    """Emit metadata pins; build flags, not these observations, enable unlock."""
    e = validate_staged_evidence(profile)
    return rehearsal_header(profile) + '\n'.join([
        '/* Prior no-unlock rehearsal plus separately reported Windows boot. */',
        '#define GU_STAGED_PROFILE_VERSION 1',
        '#define GU_STAGED_REHEARSAL_SHA256 "'+e['report_sha256']+'"',
        '#define GU_STAGED_RESTORATION_SHA256 "'+e['restoration_report_sha256']+'"',
        '#define GU_STAGED_WINDOWS_PRELOAD_SHA256 "'+e['windows_preload']['sha256']+'"',
        '#define GU_STAGED_WINDOWS_PRELOAD_BYTES '+str(e['windows_preload']['bytes'])+'ULL',
        '#define GU_STAGED_WINDOWS_BOOT_USER_CONFIRMED 1',
    ]) + '\n'
