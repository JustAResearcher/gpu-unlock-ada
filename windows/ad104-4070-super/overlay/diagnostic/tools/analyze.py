"""Validate read-only cold captures; never generates a write-enabled GPU profile."""
import argparse
import json
import re
import hashlib
from pathlib import Path


REGISTERS = (
    ('PMC_BOOT_0', 0), ('GFW_PLM', 0x118128), ('GFW_BOOT', 0x118234), ('GFW_CPUCTL', 0x110100),
    ('WPR2_LO', 0x1fa824), ('WPR2_HI', 0x1fa828), ('WPR_CFG_PLM', 0x1fa7cc), ('WPR_PLM', 0x1fa7c4),
    ('FBPA_PLM', 0x9a0148), ('FEAT_PLM', 0x823804), ('FEAT_READOUT_0', 0x823814),
    ('FEAT_READOUT_1', 0x823818), ('SS0', 0x82381c), ('SS1', 0x823820), ('FB_SIZE_MB', 0x1183a4),
    ('VGA_BASE', 0x625f04), ('MMU_PLM', 0x1fa7c8), ('MMU_LO', 0x1fa82c), ('MMU_HI', 0x1fa830),
    ('BOOTER_UCODE_FUSE', 0x824148), ('FWSEC_UCODE_FUSE', 0x8241e0), ('SEC2_CPUCTL', 0x840100))
EFI_ERROR_BIT = 1 << 63


def _hex_number(value, digits):
    if not isinstance(value, str) or not re.fullmatch(r'0x[0-9a-f]{'+str(digits)+'}', value):
        raise ValueError('Malformed hexadecimal observation')
    return int(value, 16)


def _hex_bytes(value, length):
    if not isinstance(value, str) or len(value) != length*2:
        raise ValueError('Unexpected capture payload length')
    raw = bytes.fromhex(value)
    if len(raw) != length:
        raise ValueError('Unexpected capture payload length')
    return raw


def _observation(data, kind, limit, observed):
    item = data.get(kind)
    if (not isinstance(item, dict) or type(item.get('guard_passed')) is not bool
            or type(item.get('complete')) is not bool or type(item.get('read_attempts')) is not int
            or not 0 <= item['read_attempts'] <= limit):
        raise ValueError('Malformed observation: '+kind)
    if not observed and (item['guard_passed'] or item['read_attempts'] or item.get('reason') != 'no_unique_target'):
        raise ValueError('Sampling claim without a unique target')
    if item['guard_passed']:
        if item.get('unlock_eligibility') is not False or 'reason' in item:
            raise ValueError('Unexpected sampling eligibility or failure claim')
    else:
        reasons = dict.fromkeys(('missing_protocol', 'identity_or_revision', 'memory_decode_or_bus_master_state',
                                 'unexpected_pci_location', 'bar0_relocated_or_invalid', 'bar0_descriptor_mismatch'), 0)
        if not observed:
            reasons['no_unique_target'] = 0
        if kind == 'physical_prom':
            reasons.update(unexpected_chip_for_prom=1, prom_signature_unavailable=2)
        if (item['complete'] or item.get('reason') not in reasons
                or item['read_attempts'] != reasons[item['reason']]
                or item.get('unlock_eligibility', False) is not False
                or item.get('hex') is not None or item.get('sha256') is not None
                or item.get('samples') not in (None, [])):
            raise ValueError('Inconsistent unavailable observation: '+kind)
    return item, item['read_attempts']


def _register_snapshot(item):
    if not item['guard_passed']:
        return False
    samples = item.get('samples')
    if (not isinstance(samples, list) or not 1 <= len(samples) <= len(REGISTERS)
            or _hex_number(item.get('bar0_length'), 16) != 0x1000000
            or type(item.get('pci_snapshot_unchanged')) is not bool
            or item.get('hex') is not None or item.get('sha256') is not None):
        raise ValueError('Malformed register snapshot')
    stopped = False
    for sample, (name, offset) in zip(samples, REGISTERS):
        if (stopped or not isinstance(sample, dict) or sample.get('name') != name
                or _hex_number(sample.get('offset'), 8) != offset or 'value' not in sample):
            raise ValueError('Unexpected register sequence')
        failed = bool(_hex_number(sample.get('status'), 16) & EFI_ERROR_BIT)
        value = sample['value']
        if failed:
            if value is not None:
                raise ValueError('Failed register read contains a value')
        else:
            value = _hex_number(value, 8)
        stopped = failed or value & 0xffff0000 == 0xbadf0000 or (offset == 0 and value != 0x194000a1)
    if len(samples) < len(REGISTERS) and not stopped:
        raise ValueError('Truncated register sequence without a failure')
    pci_failed = bool(_hex_number(item.get('final_pci_status'), 16) & EFI_ERROR_BIT)
    unchanged = item['pci_snapshot_unchanged']
    if pci_failed and unchanged:
        raise ValueError('Failed PCI read cannot establish stable identity')
    final_read = not stopped and unchanged and not pci_failed
    if item['read_attempts'] != len(samples) + int(final_read):
        raise ValueError('Inconsistent register read count')
    final_chip = item.get('final_chip')
    if final_chip is not None:
        _hex_number(final_chip, 8)
        if not final_read:
            raise ValueError('Final chip value without a read')
    complete = final_read and final_chip == '0x194000a1'
    if item['complete'] is not complete:
        raise ValueError('Register completion does not match observations')
    return complete


def _physical_prom(item):
    if not item['guard_passed']:
        return False
    if (type(item.get('length')) is not int or item['length'] != 1024*1024
            or _hex_number(item.get('offset'), 8) != 0x300000
            or type(item.get('identity_stable')) is not bool
            or item.get('samples') not in (None, [])):
        raise ValueError('Malformed physical PROM observation')
    failed = bool(_hex_number(item.get('data_read_status'), 16) & EFI_ERROR_BIT)
    attempts = item['read_attempts']
    if failed:
        if not 3 <= attempts <= 258:
            raise ValueError('Inconsistent failed PROM read count')
    elif attempts not in (258, 259) or (item['identity_stable'] and attempts != 259):
        raise ValueError('Inconsistent PROM verification count')
    if item['complete']:
        if failed or attempts != 259 or not item['identity_stable']:
            raise ValueError('Incomplete physical PROM checks')
        raw = _hex_bytes(item.get('hex'), item['length'])
        if raw[:2] != b'\x55\xaa' or hashlib.sha256(raw).hexdigest() != item.get('sha256'):
            raise ValueError('Physical PROM signature or hash mismatch')
    elif item.get('hex') is not None or item.get('sha256') is not None:
        raise ValueError('Incomplete physical PROM cannot export a payload')
    return item['complete']


def _sampling_guard(target, config):
    location = target.get('location')
    if (not isinstance(location, dict) or any(type(location.get(k)) is not int or location[k] != v
            for k, v in (('segment', 0), ('bus', 1), ('device', 0), ('function', 0)))
            or int.from_bytes(config[4:6], 'little') & 6 != 2 or config[16:20] != bytes.fromhex('00000080')):
        raise ValueError('Sampling bypassed the location, decode or BAR0 guard')
    bars = target.get('bars')
    if not isinstance(bars, list) or any(not isinstance(b, dict) for b in bars):
        raise ValueError('Missing sampling BAR0 descriptor')
    bar0 = [b for b in bars if type(b.get('index')) is int and b['index'] == 0]
    if len(bar0) != 1:
        raise ValueError('Missing or ambiguous sampling BAR0 descriptor')
    bar = bar0[0]
    resource = bar.get('resource')
    if (bar.get('io') is not False or bar.get('width64') is not False
            or _hex_number(bar.get('raw'), 8) != 0x80000000
            or _hex_number(bar.get('base'), 16) != 0x80000000
            or _hex_number(bar.get('descriptor_status'), 16) & EFI_ERROR_BIT
            or not isinstance(resource, dict) or resource.get('matches_config') is not True
            or type(resource.get('type')) is not int or resource['type'] != 0
            or any(_hex_number(resource.get(k), 16) != v for k, v in
                   (('minimum', 0x80000000), ('maximum', 0x80ffffff), ('translation', 0), ('length', 0x1000000)))):
        raise ValueError('Sampling bypassed the BAR0 resource guard')


def analyze(data, build_id):
    if (type(data.get('schema')) is not int or data['schema'] != 5
            or data.get('probe_version') != '0.5.0-4070s-readonly-cold'
            or data.get('probe_mode') != 'readonly-cold-4070s'):
        raise ValueError('Unsupported capture schema/version')
    if not isinstance(build_id, str) or not re.fullmatch('[0-9a-f]{64}', build_id) or data.get('build_id') != build_id:
        raise ValueError('Capture build_id does not match the selected manifest')
    for key in ('gpu_writes', 'unlock_attempted', 'hardware_support_validated'):
        if data.get(key) is not False:
            raise ValueError('Unexpected or missing diagnostic flag: '+key)
    displays = data.get('display_devices')
    if not isinstance(displays, list) or len(displays) > 32:
        raise ValueError('Malformed display inventory')
    targets = [(i, d) for i, d in enumerate(displays)
               if d.get('vendor') == '0x10de' and d.get('device_id') == '0x2783']
    exact = [(i, d) for i, d in targets
             if d.get('subsystem_vendor') == '0x1458' and d.get('subsystem_device') == '0x413b'
             and d.get('revision') == 0xa1 and d.get('exact_expected_identity') is True]
    observed = len(targets) == len(exact) == 1 and data.get('pci_enumeration_complete') is True
    if data.get('target_id_count') != len(targets) or data.get('exact_identity_count') != len(exact):
        raise ValueError('Inconsistent target counts')
    if data.get('unique_expected_identity_observed') is not observed:
        raise ValueError('Inconsistent identity summary')
    result = dict(identity_observed=observed, target_count=len(targets), target_gops=0,
                  other_display_gops=0, topology_complete=data.get('gop_enumeration_complete') is True,
                  rom_sha256=None, location=None, stable_fingerprint=None,
                  hardware_support_validated=False, unlock_ready=False)
    snapshot, register_attempts = _observation(data, 'register_snapshot', 23, observed)
    prom, prom_attempts = _observation(data, 'physical_prom', 259, observed)
    if data.get('mmio_reads') is not (register_attempts > 0 or prom_attempts > 0):
        raise ValueError('Inconsistent MMIO read flag')
    result['register_snapshot_complete'] = _register_snapshot(snapshot)
    result['physical_prom_complete'] = _physical_prom(prom)
    full_status = _hex_number(data.get('config4096_status'), 16)
    full = None
    if full_status & EFI_ERROR_BIT:
        if data.get('config4096') is not None:
            raise ValueError('Failed full configuration read contains bytes')
    else:
        full = _hex_bytes(data.get('config4096'), 4096)
    result['config4096_complete'] = full is not None
    platform=data.get('platform',{})
    if not observed and (data.get('config256') is not None or full is not None
                         or full_status != EFI_ERROR_BIT | 14 or data.get('cached_rom_hex') is not None
                         or platform.get('available') is True):
        raise ValueError('Target-specific payload without a unique exact target')
    result['platform_graph_complete']=False
    result['driver_code_captures_verified']=0
    if platform.get('available') is True and 'handles' in platform:
        handles=platform.get('handles',[])
        if not isinstance(handles,list) or len(handles)!=platform.get('handle_count') or len(handles)>4096:
            raise ValueError('Malformed UEFI handle graph')
        edge_count=0
        for i,h in enumerate(handles):
            if h.get('id')!=i or not isinstance(h.get('active_opens'),list):
                raise ValueError('Malformed UEFI graph node')
            for e in h['active_opens']:
                edge_count+=1
                if not re.fullmatch('[0-9a-f]{32}',e.get('protocol','')):
                    raise ValueError('Malformed protocol GUID')
                for k in ('agent','controller'):
                    v=e.get(k)
                    if v is not None and (isinstance(v,bool) or not isinstance(v,int) or not 0<=v<len(handles)):
                        raise ValueError('Invalid UEFI graph edge')
        if edge_count>4096 or edge_count!=platform.get('edge_count'):
            raise ValueError('Inconsistent UEFI edge count')
        agents=platform.get('gpu_agents',[])
        if not isinstance(agents,list) or len(agents)>32:
            raise ValueError('Malformed driver inventory')
        for a in agents:
            code=a.get('code')
            if code is None:continue
            length=code.get('length')
            if isinstance(length,bool) or not isinstance(length,int) or not 0<length<=512*1024:
                raise ValueError('Invalid captured code length')
            raw=code.get('hex')
            if raw is not None:
                if not isinstance(raw,str) or len(raw)!=2*length or hashlib.sha256(bytes.fromhex(raw)).hexdigest()!=code.get('loaded_sha256'):
                    raise ValueError('Loaded driver code hash mismatch')
                result['driver_code_captures_verified']+=1
        if platform.get('atomic_snapshot') is not False or platform.get('release_authorized') is not False:
            raise ValueError('Observation cannot authorize release')
        result['platform_graph_complete']=platform.get('complete') is True
    if not observed:
        return result
    index, target = exact[0]
    path = target.get('path', {})
    if target.get('location_valid') is not True or path.get('valid') is not True or path.get('multi_instance') is not False:
        raise ValueError('Target location or device path is incomplete')
    result['location'] = target['location']
    config = _hex_bytes(target.get('config64'), 64)
    if config[:4] != bytes.fromhex('de108327') or config[44:48] != bytes.fromhex('58143b41'):
        raise ValueError('Config header does not match the declared target identity')
    if config[8] != 0xa1 or config[11] != 3 or config[14] & 0x7f:
        raise ValueError('Target revision or controller class does not match')
    if register_attempts or prom_attempts:
        _sampling_guard(target, config)
    extended_hex = data.get('config256')
    extended = None
    if extended_hex is not None:
        extended = _hex_bytes(extended_hex, 256)
        if extended[:64] != config:
            raise ValueError('Extended PCI configuration does not match the target')
    if full is not None and (full[:64] != config or (extended is not None and full[:256] != extended)):
        raise ValueError('Full PCI configuration does not match the target snapshots')
    digest = target.get('cached_efi_rom', {}).get('sha256')
    if digest is not None and not re.fullmatch('[0-9a-f]{64}', digest):
        raise ValueError('Malformed cached ROM digest')
    result['rom_sha256'] = digest
    cached_hex=data.get('cached_rom_hex')
    if cached_hex is not None:
        size = target.get('cached_efi_rom', {}).get('size')
        if (type(size) is not int or not 512 <= size <= 1024*1024
                or not isinstance(cached_hex, str) or len(cached_hex) != size*2):
            raise ValueError('Unexpected cached ROM size')
        cached = bytes.fromhex(cached_hex)
        if len(cached) != size or hashlib.sha256(cached).hexdigest() != digest:
            raise ValueError('Cached ROM hash mismatch')
    gops = data.get('gops', [])
    if not isinstance(gops, list) or len(gops) > 64 or data.get('gop_count') != len(gops):
        raise ValueError('Incomplete GOP list')
    for gop in gops:
        parent = gop.get('display_parent_index')
        if parent is None or gop.get('ambiguous_parent') is not False:
            result['topology_complete'] = False
        elif isinstance(parent, bool) or not isinstance(parent, int) or not 0 <= parent < len(displays):
            raise ValueError('Invalid GOP parent index')
        elif parent == index:
            result['target_gops'] += 1
        else:
            result['other_display_gops'] += 1
    stable = {k: target.get(k) for k in ('vendor', 'device_id', 'subsystem_vendor', 'subsystem_device',
                                       'revision', 'location', 'path', 'bars', 'cached_efi_rom')}
    result['stable_fingerprint'] = json.dumps(stable, sort_keys=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('captures', nargs='+', type=Path)
    parser.add_argument('--manifest', type=Path, default=Path(__file__).resolve().parents[1]/'build/manifest.json')
    parser.add_argument('--mock', action='store_true', help='explicitly label simulated captures')
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding='utf-8-sig'))
    rows = []
    for path in args.captures:
        if not args.mock and not re.fullmatch(r'CAP\d{4}\.JSON', path.name, flags=re.I):
            parser.error('Expected CAP0001.JSON-style firmware capture; use --mock only for simulated fixtures')
        if path.stat().st_size > 8 * 1024 * 1024:
            parser.error('Capture exceeds the firmware report limit')
        try:
            result = analyze(json.loads(path.read_text(encoding='utf-8-sig')), manifest['build_id'])
        except (ValueError, KeyError, TypeError) as e:
            parser.error(str(path)+': '+str(e))
        rows.append(result)
        print(f'{path.name}: identity={result["identity_observed"]}; target GOPs={result["target_gops"]}; '
              f'other display GOPs={result["other_display_gops"]}; topology complete={result["topology_complete"]}; '
              f'registers complete={result["register_snapshot_complete"]}; PROM complete={result["physical_prom_complete"]}; '
              f'4 KiB PCI config complete={result["config4096_complete"]}; '
              f'cached ROM hash={"present" if result["rom_sha256"] else "absent"}')
    stable = all(r['identity_observed'] and r['stable_fingerprint'] == rows[0]['stable_fingerprint'] for r in rows)
    print('Capture origin:', 'HOST MOCKS ONLY' if args.mock else 'operator-provided firmware capture, not independently authenticated')
    print('Stable inventory across captures:', stable if len(rows) > 1 else 'requires another cold capture')
    print('Hardware unlock support: UNVALIDATED. No write profile generated.')


if __name__ == '__main__':
    main()
