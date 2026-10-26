import copy
import importlib.util
import json
from pathlib import Path
import struct
import unittest
from unittest import mock
import hashlib

ROOT=Path(__file__).resolve().parents[1]


def load(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/'tools'/f'{name}.py')
    module=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


verify=load('verify')
analyze=load('analyze')


def cold_fixture(rom_size=1536):
    """Synthetic analyzer input; actual UefiMain output is checked separately."""
    config=bytearray(4096)
    config[:4]=bytes.fromhex('de108327')
    config[8]=0xa1;config[11]=3;config[44:48]=bytes.fromhex('58143b41')
    config[4]=3;config[19]=0x80
    config[64:]=bytes((i*37+11)&255 for i in range(64,4096))
    rom=bytearray(rom_size)
    rom[:2]=b'\x55\xaa';rom[0x18]=0x40;rom[0x40:0x44]=b'PCIR'
    rom[0x44:0x48]=bytes.fromhex('de108327');rom[0x4a]=0x18
    rom[0x50:0x52]=(rom_size//512).to_bytes(2,'little');rom[0x55]=0x80
    target=dict(vendor='0x10de',device_id='0x2783',subsystem_vendor='0x1458',subsystem_device='0x413b',revision=0xa1,
                exact_expected_identity=True,location_valid=True,location=dict(segment=0,bus=1,device=0,function=0),
                path=dict(valid=True,multi_instance=False,hex='0101060000017fff0400'),
                bars=[dict(index=0,raw='0x80000000',base='0x0000000080000000',io=False,width64=False,
                           descriptor_status='0x0000000000000000',resource=dict(matches_config=True,type=0,
                           minimum='0x0000000080000000',maximum='0x0000000080ffffff',
                           translation='0x0000000000000000',length='0x0000000001000000'))],config64=config[:64].hex(),
                cached_efi_rom=dict(size=rom_size,sha256=hashlib.sha256(rom).hexdigest()))
    disabled=dict(guard_passed=False,reason='missing_protocol',read_attempts=0,complete=False)
    return dict(schema=5,probe_version='0.5.0-4070s-readonly-cold',probe_mode='readonly-cold-4070s',build_id='f'*64,
                gpu_writes=False,unlock_attempted=False,hardware_support_validated=False,mmio_reads=False,
                display_devices=[target],target_id_count=1,exact_identity_count=1,unique_expected_identity_observed=True,
                pci_enumeration_complete=True,gop_enumeration_complete=True,gop_count=1,
                gops=[dict(display_parent_index=0,ambiguous_parent=False)],
                register_snapshot=disabled.copy(),physical_prom=disabled.copy(),config256=config[:256].hex(),
                config4096=config.hex(),config4096_status='0x0000000000000000',cached_rom_hex=rom.hex(),
                platform=dict(available=False,complete=False))


REGISTER_NAMES=('PMC_BOOT_0','GFW_PLM','GFW_BOOT','GFW_CPUCTL','WPR2_LO','WPR2_HI','WPR_CFG_PLM','WPR_PLM',
                'FBPA_PLM','FEAT_PLM','FEAT_READOUT_0','FEAT_READOUT_1','SS0','SS1','FB_SIZE_MB','VGA_BASE',
                'MMU_PLM','MMU_LO','MMU_HI','BOOTER_UCODE_FUSE','FWSEC_UCODE_FUSE','SEC2_CPUCTL')
REGISTER_OFFSETS=(0,0x118128,0x118234,0x110100,0x1fa824,0x1fa828,0x1fa7cc,0x1fa7c4,0x9a0148,0x823804,
                  0x823814,0x823818,0x82381c,0x823820,0x1183a4,0x625f04,0x1fa7c8,0x1fa82c,0x1fa830,
                  0x824148,0x8241e0,0x840100)


def complete_fixture():
    data=cold_fixture();data['mmio_reads']=True
    data['register_snapshot']=dict(guard_passed=True,bar0_length='0x0000000001000000',
        samples=[dict(name=name,offset=f'0x{offset:08x}',status='0x0000000000000000',
                      value=f'0x{(0x194000a1 if offset==0 else offset^0x10001):08x}')
                 for name,offset in zip(REGISTER_NAMES,REGISTER_OFFSETS)],
        final_pci_status='0x0000000000000000',pci_snapshot_unchanged=True,final_chip='0x194000a1',
        read_attempts=23,complete=True,unlock_eligibility=False)
    prom=b'\x55\xaa'+bytes(1024*1024-2)
    data['physical_prom']=dict(guard_passed=True,offset='0x00300000',length=len(prom),
        data_read_status='0x0000000000000000',identity_stable=True,complete=True,read_attempts=259,
        sha256=hashlib.sha256(prom).hexdigest(),hex=prom.hex(),unlock_eligibility=False)
    return data


def without_target_payloads(data):
    data['config256']=data['config4096']=data['cached_rom_hex']=None
    data['config4096_status']='0x800000000000000e'
    data['mmio_reads']=False
    for kind in ('register_snapshot','physical_prom'):
        data[kind]=dict(guard_passed=False,reason='no_unique_target',read_attempts=0,complete=False)


class ToolTests(unittest.TestCase):
    def test_pe_accepts_built_image(self):
        verify.validate_pe((ROOT/'build/AD104-PROBE.EFI').read_bytes())

    def test_pe_rejects_mutations(self):
        original=(ROOT/'build/AD104-PROBE.EFI').read_bytes()
        pe=struct.unpack_from('<I',original,0x3c)[0]
        opt=pe+24
        cases=[(0,b'ZZ'),(pe+4,struct.pack('<H',0x14c)),
               (opt+68,struct.pack('<H',3)),(opt+16,b'\0'*4),
               (opt+112+8,struct.pack('<II',0x1000,64)),
               (opt+112+5*8,b'\0'*8),(opt+112+9*8,struct.pack('<II',0x1000,64)),
               (opt+112+14*8,struct.pack('<II',0x1000,64)),
               (0x3c,struct.pack('<I',0xffffffff))]
        for offset,patch in cases:
            with self.subTest(offset=offset):
                data=bytearray(original);data[offset:offset+len(patch)]=patch
                with self.assertRaises(ValueError):
                    verify.validate_pe(data)
        with self.assertRaises(ValueError):
            verify.validate_pe(original[:100])

    def test_source_rejects_forbidden_calls(self):
        calls=['pci->Mem.Read(', 'pci->Mem.Write(', 'pci->Io.Read(', 'pci->Io.Write(',
               'pci -> Mem . Read(', '(*pci).Io.Write(', 'pci-> /* comment */ Mem.Read(',
               'pci->Mem.\\\nRead(', 'fn = pci->Mem.Read;', 'pci->Pci.Write(', 'pci.Pci . Write(']
        calls.extend('pci->'+name+'(' for name in ('Map','Unmap','AllocateBuffer','FreeBuffer','Attributes','SetBarAttributes','PollMem','PollIo','CopyMem'))
        calls.extend('bs->'+name+'(' for name in ('DisconnectController','ConnectController','StartImage','LoadImage','UnloadImage','ExitBootServices','CloseProtocol'))
        calls.extend(('rs->SetVariable(', 'rs->ResetSystem(', 'gop->SetMode(', 'gop->Blt(', 'binding->Stop(', 'binding->Start('))
        for call in calls:
            with self.subTest(call=call),self.assertRaises(ValueError):
                verify.validate_source(call)
        verify.validate_source('pci->Pci.Read(pci); st->ConOut->OutputString(st->ConOut, text);')
        verify.validate_source('/* pci->Mem.Read() is forbidden. */ bs->OpenProtocolInformation(h);')

    def test_mmio_opt_in_allows_only_reads(self):
        for call in ('pci->Mem.Read(', 'pci -> Mem . Read(', '(*pci).Mem.Read(',
                     'pci-> /* comment */ Mem.Read(', 'pci->Mem.\\\nRead(', 'fn = pci->Mem.Read;'):
            with self.subTest(call=call):
                verify.validate_source(call,allow_mmio_reads=True)
                with self.assertRaises(ValueError):
                    verify.validate_source(call)
        forbidden=['pci->Mem.Write(', 'pci->Io.Read(', 'pci->Io.Write(', 'pci->Pci.Write(',
                   '(*pci).Mem.Write(', 'pci -> /* comment */ Mem . Write(', 'pci->Mem.\\\nWrite(',
                   'fn = pci->Mem.Write;', 'copy = pci->Mem;', 'copy = pci->Io;']
        forbidden.extend('pci->'+name+';' for name in ('Map','Unmap','AllocateBuffer','FreeBuffer','Attributes',
                         'SetBarAttributes','PollMem','PollIo','CopyMem'))
        forbidden.extend('bs->'+name+'(' for name in ('DisconnectController','ConnectController','StartImage','LoadImage',
                         'UnloadImage','ExitBootServices','CloseProtocol'))
        forbidden.extend(('rs->SetVariable(', 'rs->ResetSystem(', 'gop->SetMode(', 'gop->Blt(',
                          'binding->Stop(', 'binding->Start(', 'binding->Supported('))
        for call in forbidden:
            for allow in (False,True):
                with self.subTest(call=call,allow=allow),self.assertRaises(ValueError):
                    verify.validate_source(call,allow_mmio_reads=allow)

    def test_source_allows_typed_open_protocol_attributes(self):
        metadata='EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *info=NULL; UINT32 attr=info[k].Attributes;'
        for allow in (False,True):
            with self.subTest(allow=allow):
                verify.validate_source(metadata,allow_mmio_reads=allow)
        verify.validate_source((ROOT/'src/platform.c').read_text())

    def test_source_rejects_pci_attributes_calls_and_references(self):
        prefix='EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *info=NULL; UINT32 attr=info[k].Attributes; '
        callbacks=('pci->Attributes(pci, operation, attributes, result);', 'fn=pci->Attributes;',
                   'fn=pci.Attributes;', 'fn=(*pci).Attributes;', 'fn=pci[0].Attributes;',
                   'pci -> /* callback */ Attributes(pci);', 'fn=pci->\\\nAttributes;',
                   'info[k].Attributes(info);')
        for callback in callbacks:
            for allow in (False,True):
                with self.subTest(callback=callback,allow=allow),self.assertRaises(ValueError):
                    verify.validate_source(prefix+callback,allow_mmio_reads=allow)
        with self.assertRaises(ValueError):
            verify.validate_source('EFI_PCI_IO_PROTOCOL *info; fn=info[k].Attributes;')

    def test_source_allows_typed_stop_address_metadata(self):
        prefix='EFI_DRIVER_BINDING_PROTOCOL *binding=NULL; '
        metadata=('if((UINTN)binding->Stop>=(UINTN)image->ImageBase&&(UINTN)binding->Stop-(UINTN)image->ImageBase<image->ImageSize) present=1;',
                  'TextHex(t,(UINTN)binding->Stop-(UINTN)image->ImageBase,16);')
        for expression in metadata:
            for allow in (False,True):
                with self.subTest(expression=expression,allow=allow):
                    verify.validate_source(prefix+expression,allow_mmio_reads=allow)

    def test_source_rejects_stop_invocation_and_aliases(self):
        prefix='EFI_DRIVER_BINDING_PROTOCOL *binding=NULL; '
        callbacks=('binding->Stop(binding,handle,0,NULL);', '(binding->Stop)(binding,handle,0,NULL);',
                   'value=(UINTN)binding->Stop(binding,handle,0,NULL);',
                   'value=(UINTN)(binding->Stop(binding,handle,0,NULL));',
                   'fn=binding->Stop;', 'fn=(*binding).Stop;', 'fn=(UINTN)binding->Stop;',
                   'fn=(EFI_DRIVER_BINDING_STOP)binding->Stop;', 'fn=&binding->Stop;',
                   'value=(UINTN)binding->Start;', 'value=(UINTN)binding->Supported;',
                   'value=(UINTN)bs->DisconnectController;', 'value=(UINTN)rs->ResetSystem;')
        for callback in callbacks:
            for allow in (False,True):
                with self.subTest(callback=callback,allow=allow),self.assertRaises(ValueError):
                    verify.validate_source(prefix+callback,allow_mmio_reads=allow)
        with self.assertRaises(ValueError):
            verify.validate_source('OTHER_PROTOCOL *binding; value=(UINTN)binding->Stop-(UINTN)image->ImageBase;')

    def test_source_rejects_metadata_exception_alias_and_scope_regressions(self):
        cases=(
            'EFI_DRIVER_BINDING_PROTOCOL *binding; EFI_DRIVER_BINDING_STOP fn=(EFI_DRIVER_BINDING_STOP)((UINTN)binding->Stop-0); fn(binding,handle,0,NULL);',
            'void inventory(void){EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *info; UINT32 attr=info[k].Attributes;} EFI_STATUS invoke(EFI_PCI_IO_PROTOCOL *info){EFI_PCI_IO_PROTOCOL_ATTRIBUTES fn=info[0].Attributes; return fn(info,operation,attributes,result);}',
        )
        for source in cases:
            for allow in (False,True):
                with self.subTest(source=source,allow=allow),self.assertRaises(ValueError):
                    verify.validate_source(source,allow_mmio_reads=allow)

    def test_verifier_opts_in_only_the_sampler_source(self):
        with mock.patch.object(verify,'validate_source') as source_check, mock.patch.object(verify,'validate_capture'):
            verify.main()
        paths=sorted(p for p in (ROOT/'src').iterdir() if p.suffix in ('.c','.h'))
        self.assertEqual(source_check.call_count,len(paths))
        for call,path in zip(source_check.call_args_list,paths):
            self.assertEqual(call.args[0],path.read_text())
            self.assertEqual(call.kwargs.get('allow_mmio_reads',False),path.name=='sample.c')

    def test_capture_accepts_mock_as_inventory_only(self):
        data=json.loads((ROOT/'build/host-capture.json').read_text())
        result=analyze.analyze(data,data['build_id'])
        self.assertTrue(result['identity_observed'])
        self.assertEqual(result['target_gops'],1)
        self.assertFalse(result['unlock_ready'])
        self.assertFalse(result['hardware_support_validated'])
        self.assertTrue(result['register_snapshot_complete'])
        self.assertEqual(len(data['cached_rom_hex']),1536*2)
        self.assertTrue(result['physical_prom_complete'])
        self.assertEqual(len(data['config256']),512)
        self.assertEqual(len(data['config4096']),8192)
        self.assertTrue(result['platform_graph_complete'])
        self.assertEqual(result['driver_code_captures_verified'],1)

    def test_cold_fixture_accepts_actual_bounded_rom_sizes(self):
        for size in (512,1536,1024*1024):
            with self.subTest(size=size):
                data=cold_fixture(size)
                result=analyze.analyze(data,data['build_id'])
                self.assertTrue(result['identity_observed'])
                self.assertFalse(result['register_snapshot_complete'])
                self.assertFalse(result['physical_prom_complete'])
                self.assertFalse(result['unlock_ready'])
                self.assertFalse(result['hardware_support_validated'])
                self.assertEqual(result['rom_sha256'],data['display_devices'][0]['cached_efi_rom']['sha256'])

    def test_optional_payloads_can_be_absent(self):
        data=cold_fixture();data['config256']=data['config4096']=data['cached_rom_hex']=None
        data['config4096_status']='0x8000000000000007'
        data['display_devices'][0]['cached_efi_rom']['sha256']=None
        result=analyze.analyze(data,data['build_id'])
        self.assertTrue(result['identity_observed'])
        self.assertFalse(result['unlock_ready'])

    def test_capture_rejects_mutations(self):
        data=cold_fixture()
        with self.assertRaises(ValueError):
            analyze.analyze(data,'incorrect-build')
        for field in ('gpu_writes','unlock_attempted','hardware_support_validated','mmio_reads'):
            changed=copy.deepcopy(data);changed[field]=True
            with self.subTest(field=field),self.assertRaises(ValueError):
                analyze.analyze(changed,data['build_id'])
        changed=copy.deepcopy(data);changed['target_id_count']=2
        with self.assertRaises(ValueError):
            analyze.analyze(changed,data['build_id'])

        changed=copy.deepcopy(data);changed['display_devices'][0]['config64']='00'*64
        with self.assertRaises(ValueError):
            analyze.analyze(changed,data['build_id'])
        changed=copy.deepcopy(data);changed['cached_rom_hex']='00'+changed['cached_rom_hex'][2:]
        with self.assertRaises(ValueError):
            analyze.analyze(changed,data['build_id'])
        changed=copy.deepcopy(data);changed['register_snapshot']['read_attempts']=999
        with self.assertRaises(ValueError):
            analyze.analyze(changed,data['build_id'])
        for key,value in (('schema',3),('probe_version','0.4.0'),('probe_mode','metadata-only-other')):
            changed=copy.deepcopy(data);changed[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):
                analyze.analyze(changed,data['build_id'])
        for key in ('probe_mode','gpu_writes','mmio_reads'):
            changed=copy.deepcopy(data);del changed[key]
            with self.subTest(missing=key),self.assertRaises(ValueError):
                analyze.analyze(changed,data['build_id'])

    def test_identity_fields_and_both_config_snapshots_are_bound(self):
        data=cold_fixture()
        for field,value in (('vendor','0x1234'),('device_id','0x2782'),('subsystem_vendor','0x1462'),
                            ('subsystem_device','0x5132'),('revision',0xa2),('exact_expected_identity',False)):
            changed=copy.deepcopy(data);changed['display_devices'][0][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):
                analyze.analyze(changed,data['build_id'])
        for position in (0,2,8,11,14,44,46):
            for field in ('config64','config256','config4096'):
                changed=copy.deepcopy(data)
                holder=changed['display_devices'][0] if field=='config64' else changed
                config=bytearray.fromhex(holder[field]);config[position]^=1;holder[field]=config.hex()
                with self.subTest(position=position,field=field),self.assertRaises(ValueError):
                    analyze.analyze(changed,data['build_id'])
        changed=copy.deepcopy(data);changed['config256']=changed['config256'][:-2]
        with self.assertRaises(ValueError):
            analyze.analyze(changed,data['build_id'])

    def test_non_target_and_ambiguous_inventory_cannot_export_payloads(self):
        ownership=dict(available=True,complete=True,atomic_snapshot=False,release_authorized=False,
                       target=0,conout=0,conin=None,stderr=None,handle_count=2,edge_count=1,
                       handles=[dict(id=0,gpu_descendant=True,path='0101060000017fff0400',
                                     active_opens=[dict(protocol='0'*32,agent=1,controller=0,attributes='0x00000010',count=1)]),
                                dict(id=1,gpu_descendant=False,path=None,active_opens=[])],
                       gpu_agents=[dict(agent=1,code=dict(length=1,hex='00',loaded_sha256=hashlib.sha256(b'\0').hexdigest()))])
        data=cold_fixture();data['platform']=ownership
        with self.subTest(ownership='unique target'):
            self.assertEqual(analyze.analyze(data,data['build_id'])['driver_code_captures_verified'],1)
        for field,offset,value in (('vendor',0,'0x1234'),('device_id',2,'0x2782'),
                                   ('subsystem_vendor',44,'0x1462'),('subsystem_device',46,'0x5132'),('revision',8,0xa2)):
            data=cold_fixture();target=data['display_devices'][0]
            config=bytearray.fromhex(target['config64']);target[field]=value
            if field=='revision':config[offset]=value
            else:config[offset:offset+2]=int(value,16).to_bytes(2,'little')
            target['config64']=config.hex();target['exact_expected_identity']=False;target['cached_efi_rom']['sha256']=None
            data['target_id_count']=0 if field in ('vendor','device_id') else 1
            data['exact_identity_count']=0;data['unique_expected_identity_observed']=False
            payloads={key:data[key] for key in ('config256','config4096','cached_rom_hex')}
            without_target_payloads(data)
            with self.subTest(field=field):
                self.assertFalse(analyze.analyze(data,data['build_id'])['identity_observed'])
            for key,value in payloads.items():
                changed=copy.deepcopy(data);changed[key]=value
                with self.subTest(field=field,payload=key),self.assertRaises(ValueError):
                    analyze.analyze(changed,data['build_id'])
            changed=copy.deepcopy(data);changed['platform']=ownership
            with self.subTest(field=field,payload='platform'),self.assertRaises(ValueError):
                analyze.analyze(changed,data['build_id'])
        data=cold_fixture();data['display_devices'].append(copy.deepcopy(data['display_devices'][0]))
        data['target_id_count']=data['exact_identity_count']=2;data['unique_expected_identity_observed']=False
        with self.assertRaises(ValueError):
            analyze.analyze(data,data['build_id'])
        without_target_payloads(data)
        self.assertFalse(analyze.analyze(data,data['build_id'])['identity_observed'])
        data['platform']=ownership
        with self.assertRaises(ValueError):
            analyze.analyze(data,data['build_id'])

    def test_rejects_fabricated_completion_without_observations(self):
        for kind in ('register_snapshot','physical_prom'):
            cases=(('guard_passed',True),('complete',True),('read_attempts',-1),('read_attempts',True),
                   ('read_attempts','0'),('reason','capture_completed'),('hex','00'),('sha256','0'*64),
                   ('samples',[dict(offset='0x00000000',value='0x194000a1',status='0x0000000000000000')]))
            for field,value in cases:
                data=cold_fixture();data[kind][field]=value
                with self.subTest(kind=kind,field=field,value=value),self.assertRaises(ValueError):
                    analyze.analyze(data,data['build_id'])
            data=cold_fixture();del data[kind]['complete']
            with self.subTest(kind=kind,missing='complete'),self.assertRaises(ValueError):
                analyze.analyze(data,data['build_id'])

    def test_cached_rom_metadata_length_and_hash_are_bound(self):
        for key,value in (('size',512),('size',True),('size',-1),('size',1024*1024+1),('sha256',None),('sha256','0'*64)):
            data=cold_fixture();data['display_devices'][0]['cached_efi_rom'][key]=value
            with self.subTest(key=key,value=value),self.assertRaises(ValueError):
                analyze.analyze(data,data['build_id'])
        for raw in ('','xyz','00'*512):
            data=cold_fixture();data['cached_rom_hex']=raw
            with self.subTest(raw=raw[:8]),self.assertRaises(ValueError):
                analyze.analyze(data,data['build_id'])
        data=cold_fixture(1024*1024+512)
        with self.assertRaises(ValueError):
            analyze.analyze(data,data['build_id'])

    def test_complete_cold_observations_do_not_authorize_unlock(self):
        data=complete_fixture();result=analyze.analyze(data,data['build_id'])
        self.assertTrue(result['identity_observed'])
        self.assertTrue(result['register_snapshot_complete'])
        self.assertTrue(result['physical_prom_complete'])
        self.assertFalse(result['hardware_support_validated'])
        self.assertFalse(result['unlock_ready'])

    def test_actual_partial_application_captures_remain_incomplete(self):
        for name,registers,prom in (('register-error',False,True),('prom-error',True,False),
                                    ('decode-disabled',False,False),('wrong-chip',False,False)):
            with self.subTest(name=name):
                data=json.loads((ROOT/'build'/f'host-capture-{name}.json').read_text())
                result=analyze.analyze(data,data['build_id'])
                self.assertEqual(result['register_snapshot_complete'],registers)
                self.assertEqual(result['physical_prom_complete'],prom)
                self.assertFalse(result['unlock_ready'])
        for name in ('config-error','config-drift'):
            with self.subTest(name=name):
                data=json.loads((ROOT/'build'/f'host-capture-{name}.json').read_text())
                self.assertIsNone(data['config4096'])
                self.assertFalse(analyze.analyze(data,data['build_id'])['unlock_ready'])

    def test_partial_register_observations_are_valid_but_never_complete(self):
        for cause in ('read-error','sentinel','wrong-chip','config-drift','config-error','chip-drift','chip-error'):
            data=complete_fixture();snapshot=data['register_snapshot']
            snapshot['complete']=False;snapshot['final_chip']=None
            if cause in ('read-error','sentinel'):
                snapshot['samples']=snapshot['samples'][:2];snapshot['read_attempts']=2
                snapshot['samples'][-1].update(status='0x8000000000000007' if cause=='read-error' else '0x0000000000000000',
                                              value=None if cause=='read-error' else '0xbadf0001')
            elif cause=='wrong-chip':
                snapshot['samples']=snapshot['samples'][:1];snapshot['read_attempts']=1
                snapshot['samples'][0]['value']='0x194000a2'
            elif cause in ('config-drift','config-error'):
                snapshot['read_attempts']=22;snapshot['pci_snapshot_unchanged']=False
                if cause=='config-error':snapshot['final_pci_status']='0x8000000000000007'
            elif cause=='chip-drift':snapshot['final_chip']='0x194000a2'
            with self.subTest(cause=cause):
                result=analyze.analyze(data,data['build_id'])
                self.assertFalse(result['register_snapshot_complete'])
                self.assertTrue(result['physical_prom_complete'])
                self.assertFalse(result['unlock_ready'])

    def test_partial_prom_observations_never_export_bytes(self):
        for cause in ('chip','signature','read-error','identity-drift','data-drift','chip-error'):
            data=complete_fixture();prom=data['physical_prom']
            if cause in ('chip','signature'):
                prom=dict(guard_passed=False,complete=False,read_attempts=1 if cause=='chip' else 2,
                          reason='unexpected_chip_for_prom' if cause=='chip' else 'prom_signature_unavailable')
                data['physical_prom']=prom
            else:
                prom.update(complete=False,hex=None,sha256=None)
                if cause=='read-error':prom.update(data_read_status='0x8000000000000007',read_attempts=4)
                if cause=='identity-drift':prom.update(identity_stable=False,read_attempts=258)
                if cause=='chip-error':prom['identity_stable']=False
            with self.subTest(cause=cause):
                result=analyze.analyze(data,data['build_id'])
                self.assertFalse(result['physical_prom_complete'])
                self.assertFalse(result['unlock_ready'])
            for key,value in (('hex','55aa'),('sha256','0'*64)):
                invalid=copy.deepcopy(data);invalid['physical_prom'][key]=value
                with self.subTest(cause=cause,payload=key),self.assertRaises(ValueError):
                    analyze.analyze(invalid,data['build_id'])

    def test_completed_register_claim_requires_all_observations(self):
        cases=[('guard_passed',False),('complete',1),('read_attempts',True),('read_attempts',22),('read_attempts',24),
               ('bar0_length','0x0000000002000000'),('pci_snapshot_unchanged',False),('pci_snapshot_unchanged',1),
               ('final_pci_status','0x8000000000000007'),('final_chip',None),('final_chip','0x194000a2'),
               ('unlock_eligibility',True)]
        for key,value in cases:
            data=complete_fixture();data['register_snapshot'][key]=value
            with self.subTest(key=key,value=value),self.assertRaises(ValueError):
                analyze.analyze(data,data['build_id'])
        for mutation in ('missing','extra','duplicate','reordered','offset','error','null','sentinel','wrong-chip'):
            data=complete_fixture();samples=data['register_snapshot']['samples']
            if mutation=='missing':samples.pop()
            if mutation=='extra':samples.append(copy.deepcopy(samples[-1]))
            if mutation=='duplicate':samples[1]=copy.deepcopy(samples[0])
            if mutation=='reordered':samples[1],samples[2]=samples[2],samples[1]
            if mutation=='offset':samples[1]['offset']='0x0011812c'
            if mutation=='error':samples[1]['status']='0x8000000000000007'
            if mutation=='null':samples[1]['value']=None
            if mutation=='sentinel':samples[1]['value']='0xbadf0001'
            if mutation=='wrong-chip':samples[0]['value']='0x194000a2'
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):
                analyze.analyze(data,data['build_id'])

    def test_completed_prom_claim_requires_bounded_verified_payload(self):
        for key,value in (('guard_passed',False),('complete',1),('length',True),('length',1048575),('length',1048577),
                          ('offset','0x00300004'),('identity_stable',False),('identity_stable',1),
                          ('data_read_status','0x8000000000000007'),('read_attempts',258),('read_attempts',260),
                          ('read_attempts',True),('sha256',None),('sha256','0'*64),('hex',None),('hex','55aa'),
                          ('unlock_eligibility',True)):
            data=complete_fixture();data['physical_prom'][key]=value
            with self.subTest(key=key,value=value),self.assertRaises(ValueError):
                analyze.analyze(data,data['build_id'])
        for mutation in ('signature','short','long','bad-hex','hash'):
            data=complete_fixture();prom=data['physical_prom'];raw=bytearray.fromhex(prom['hex'])
            if mutation=='signature':raw[:2]=b'\0\0'
            if mutation=='short':raw.pop()
            if mutation=='long':raw.append(0)
            if mutation=='hash':raw[-1]^=1
            prom['hex']=raw.hex();prom['sha256']=hashlib.sha256(raw).hexdigest()
            if mutation=='bad-hex':prom['hex']='z'+prom['hex'][1:]
            if mutation=='hash':prom['sha256']='0'*64
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):
                analyze.analyze(data,data['build_id'])

    def test_full_config_has_status_length_and_shared_prefix_binding(self):
        for missing_256 in (False,True):
            data=cold_fixture()
            if missing_256:data['config256']=None
            self.assertFalse(analyze.analyze(data,data['build_id'])['unlock_ready'])
        for raw in ('','00'*4095,'00'*4097,'z'*8192,None,True):
            data=cold_fixture();data['config4096']=raw
            with self.subTest(raw_type=type(raw),length=len(raw) if isinstance(raw,str) else None),self.assertRaises(ValueError):
                analyze.analyze(data,data['build_id'])
        for position in (4,8,16,44,63,64,128,255):
            data=cold_fixture();full=bytearray.fromhex(data['config4096']);full[position]^=1;data['config4096']=full.hex()
            with self.subTest(position=position),self.assertRaises(ValueError):
                analyze.analyze(data,data['build_id'])
        for status in ('0x8000000000000007','0x8000000000000021',None,True,0,'garbage'):
            data=cold_fixture();data['config4096_status']=status
            with self.subTest(status=status),self.assertRaises(ValueError):
                analyze.analyze(data,data['build_id'])
        for status in ('0x8000000000000007','0x8000000000000021'):
            data=cold_fixture();data['config4096']=None;data['config4096_status']=status
            self.assertFalse(analyze.analyze(data,data['build_id'])['unlock_ready'])
        data=cold_fixture();del data['config4096_status']
        with self.assertRaises(ValueError):
            analyze.analyze(data,data['build_id'])

    def test_sampling_claims_require_unique_target_and_truthful_mmio_summary(self):
        for field in ('gpu_writes','unlock_attempted','hardware_support_validated'):
            data=complete_fixture();data[field]=True
            with self.subTest(field=field),self.assertRaises(ValueError):
                analyze.analyze(data,data['build_id'])
        for value in (False,None,1):
            data=complete_fixture();data['mmio_reads']=value
            with self.subTest(mmio_reads=value),self.assertRaises(ValueError):
                analyze.analyze(data,data['build_id'])
        for kind in ('register_snapshot','physical_prom'):
            complete=complete_fixture();data=copy.deepcopy(complete)
            data['display_devices'].append(copy.deepcopy(data['display_devices'][0]))
            data['target_id_count']=data['exact_identity_count']=2;data['unique_expected_identity_observed']=False
            without_target_payloads(data);data[kind]=complete[kind];data['mmio_reads']=True
            with self.subTest(kind=kind),self.assertRaises(ValueError):
                analyze.analyze(data,data['build_id'])

    def test_completed_samples_cannot_bypass_the_declared_hardware_guard(self):
        for position,value in ((4,1),(4,7),(19,0x90)):
            data=complete_fixture()
            for key,holder in (('config64',data['display_devices'][0]),('config256',data),('config4096',data)):
                raw=bytearray.fromhex(holder[key]);raw[position]=value;holder[key]=raw.hex()
            with self.subTest(position=position,value=value),self.assertRaises(ValueError):
                analyze.analyze(data,data['build_id'])
        for field,value in (('segment',1),('bus',2),('device',1),('function',1)):
            data=complete_fixture();data['display_devices'][0]['location'][field]=value
            with self.subTest(location=field),self.assertRaises(ValueError):
                analyze.analyze(data,data['build_id'])
        for field,value in (('matches_config',False),('type',1),('minimum','0x0000000090000000'),
                            ('maximum','0x0000000081ffffff'),('translation','0x0000000000000001'),
                            ('length','0x0000000002000000')):
            data=complete_fixture();data['display_devices'][0]['bars'][0]['resource'][field]=value
            with self.subTest(resource=field),self.assertRaises(ValueError):
                analyze.analyze(data,data['build_id'])

    def test_driver_code_hash(self):
        data=cold_fixture()
        data['platform']={'available':True,'handles':[], 'handle_count':0,'edge_count':0,
                         'gpu_agents':[{'code':{'length':1,'hex':'00','loaded_sha256':hashlib.sha256(b'\0').hexdigest()}}],
                         'complete':False,'atomic_snapshot':False,'release_authorized':False}
        self.assertEqual(analyze.analyze(data,data['build_id'])['driver_code_captures_verified'],1)
        data['platform']['gpu_agents'][0]['code']['hex']='01'
        with self.assertRaises(ValueError):
            analyze.analyze(data,data['build_id'])



if __name__=='__main__':
    unittest.main()
