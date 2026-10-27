"""Analyze inventory captures; never generates a write-enabled GPU profile."""
import argparse
import json
import re
import hashlib
from pathlib import Path


def analyze(data, build_id):
    if data.get('schema') != 4 or data.get('probe_version') != '0.4.0':
        raise ValueError('Unsupported capture schema/version')
    if data.get('build_id') != build_id:
        raise ValueError('Capture build_id does not match the selected manifest')
    for key in ('gpu_writes', 'unlock_attempted', 'hardware_support_validated'):
        if data.get(key) is not False:
            raise ValueError('Unexpected or missing diagnostic flag: '+key)
    displays = data.get('display_devices')
    if not isinstance(displays, list) or len(displays) > 32:
        raise ValueError('Malformed display inventory')
    targets = [(i, d) for i, d in enumerate(displays)
               if d.get('vendor') == '0x10de' and d.get('device_id') == '0x2782']
    exact = [(i, d) for i, d in targets
             if d.get('subsystem_vendor') == '0x1462' and d.get('subsystem_device') == '0x5132'
             and d.get('exact_expected_identity') is True]
    observed = len(targets) == len(exact) == 1 and data.get('pci_enumeration_complete') is True
    if data.get('target_id_count') != len(targets) or data.get('exact_identity_count') != len(exact):
        raise ValueError('Inconsistent target counts')
    if data.get('unique_expected_identity_observed') is not observed:
        raise ValueError('Inconsistent identity summary')
    result = dict(identity_observed=observed, target_count=len(targets), target_gops=0,
                  other_display_gops=0, topology_complete=data.get('gop_enumeration_complete') is True,
                  rom_sha256=None, location=None, stable_fingerprint=None,
                  hardware_support_validated=False, unlock_ready=False)
    snapshot=data.get('register_snapshot',{})
    attempts=snapshot.get('read_attempts')
    if not isinstance(attempts,int) or isinstance(attempts,bool) or not 0<=attempts<=23:
        raise ValueError('Invalid register read count')
    prom=data.get('physical_prom',{})
    prom_attempts=prom.get('read_attempts')
    if not isinstance(prom_attempts,int) or isinstance(prom_attempts,bool) or not 0<=prom_attempts<=259:
        raise ValueError('Invalid PROM read count')
    if data.get('mmio_reads') is not (attempts>0 or prom_attempts>0):
        raise ValueError('Inconsistent MMIO read flag')
    if not observed and (attempts or prom_attempts):
        raise ValueError('Read attempted without a unique target')
    result['register_snapshot_complete']=snapshot.get('complete') is True
    samples=snapshot.get('samples',[])
    offsets=[0,0x118128,0x118234,0x110100,0x1fa824,0x1fa828,0x1fa7cc,0x1fa7c4,0x9a0148,0x823804,0x823814,0x823818,0x82381c,0x823820,0x1183a4,0x625f04,0x1fa7c8,0x1fa82c,0x1fa830,0x824148,0x8241e0,0x840100]
    if not isinstance(samples,list) or len(samples)>22 or [int(s['offset'],16) for s in samples]!=offsets[:len(samples)]:
        raise ValueError('Unexpected partial register allowlist')
    if attempts not in (len(samples),len(samples)+1):
        raise ValueError('Inconsistent register sample count')
    for sample in samples:
        if sample.get('value') is not None and not re.fullmatch(r'0x[0-9a-f]{8}',sample['value']):
            raise ValueError('Malformed sampled register')
    platform=data.get('platform',{})
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
    result['physical_prom_complete']=prom.get('complete') is True
    if result['physical_prom_complete']:
        if prom_attempts!=259 or prom.get('guard_passed') is not True or prom.get('identity_stable') is not True or prom.get('length')!=1048576 or prom.get('offset')!='0x00300000':
            raise ValueError('Inconsistent full PROM capture')
        raw=prom.get('hex')
        if not isinstance(raw,str) or len(raw)!=2*1048576:
            raise ValueError('Incomplete PROM bytes')
        if hashlib.sha256(bytes.fromhex(raw)).hexdigest()!=prom.get('sha256'):
            raise ValueError('PROM hash mismatch')
    if result['register_snapshot_complete']:
        if attempts!=23 or snapshot.get('guard_passed') is not True or snapshot.get('pci_snapshot_unchanged') is not True:
            raise ValueError('Inconsistent completed snapshot')
        samples=snapshot.get('samples',[])
        offsets=[0,0x118128,0x118234,0x110100,0x1fa824,0x1fa828,0x1fa7cc,0x1fa7c4,0x9a0148,0x823804,0x823814,0x823818,0x82381c,0x823820,0x1183a4,0x625f04,0x1fa7c8,0x1fa82c,0x1fa830,0x824148,0x8241e0,0x840100]
        if len(samples)!=22 or [int(s['offset'],16) for s in samples]!=offsets:
            raise ValueError('Unexpected register allowlist')
        if any(s.get('status')!='0x0000000000000000' or not re.fullmatch(r'0x[0-9a-f]{8}',s.get('value','')) for s in samples):
            raise ValueError('Invalid register result')
        if snapshot.get('final_chip')!=samples[0]['value']:
            raise ValueError('Chip identity changed')
    if not observed:
        return result
    index, target = exact[0]
    path = target.get('path', {})
    if target.get('location_valid') is not True or path.get('valid') is not True or path.get('multi_instance') is not False:
        raise ValueError('Target location or device path is incomplete')
    result['location'] = target['location']
    config = bytes.fromhex(target.get('config64', ''))
    if len(config) != 64 or config[:4] != bytes.fromhex('de108227') or config[44:48] != bytes.fromhex('62143251'):
        raise ValueError('Config header does not match the declared target identity')
    if config[11] != 3 or config[14] & 0x7f:
        raise ValueError('Target is not a type-0 display controller')
    digest = target.get('cached_efi_rom', {}).get('sha256')
    if digest is not None and not re.fullmatch('[0-9a-f]{64}', digest):
        raise ValueError('Malformed cached ROM digest')
    result['rom_sha256'] = digest
    cached_hex=data.get('cached_rom_hex')
    if cached_hex is not None:
        if not isinstance(cached_hex,str) or len(cached_hex)!=150016*2:
            raise ValueError('Unexpected cached ROM size')
        cached=bytes.fromhex(cached_hex)
        if hashlib.sha256(cached).hexdigest()!=digest:
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
              f'cached ROM hash={"present" if result["rom_sha256"] else "absent"}')
    stable = all(r['identity_observed'] and r['stable_fingerprint'] == rows[0]['stable_fingerprint'] for r in rows)
    print('Capture origin:', 'HOST MOCKS ONLY' if args.mock else 'operator-provided firmware capture, not independently authenticated')
    print('Stable inventory across captures:', stable if len(rows) > 1 else 'requires another cold capture')
    print('Hardware unlock support: UNVALIDATED. No write profile generated.')


if __name__ == '__main__':
    main()
