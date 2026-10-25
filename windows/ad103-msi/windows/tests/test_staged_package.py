# SPDX-License-Identifier: GPL-2.0-only
"""Reject stale, skipped, or mismatched staged release evidence.

All reports, PE bytes and source files here are synthetic. The fixture image is
never executed or distributed and contains no NVIDIA asset bytes.
"""
from __future__ import annotations

import copy
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

WIN = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(WIN))
from board_profile import header as board_header
from build_staged import (MODE,VERSION,DEFINES,SOURCES,REQUIRED_ASSETS,
                          build_source_hashes,verify_assets)
from package_diagnostic import HOST_REQUIRED_SOURCES
import package_staged
from package_staged import check_inputs,digest,CASES,USB_FILES,NAME
from rehearsal_profile import parse_capture,bind_restoration_report
from staged_profile import bind_rehearsal_success,staged_header
from validate_staged import (validation_source_hashes,EXPECTED_CHECKS,BENCHMARK_FILES,
                             REAL_TRANSACTION_MARKER,REAL_ASSET_IO_MARKER,verify_critical_host_checks)
from test_rehearsal_profile import fixture,restoration_fixture
from test_staged_profile import success_fixture


class StagedPackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        image = bytearray(1024); image[:2] = b'MZ'
        struct.pack_into('<I',image,60,128); image[128:132] = b'PE\0\0'
        struct.pack_into('<HH',image,132,0x8664,1); struct.pack_into('<H',image,148,240)
        opt = 152; struct.pack_into('<H',image,opt,0x20b)
        struct.pack_into('<I',image,opt+16,0x1000); struct.pack_into('<HH',image,opt+68,10,0x140)
        struct.pack_into('<I',image,opt+112+5*8+4,12)
        section = opt+240; struct.pack_into('<II',image,section+8,512,0x1000)
        struct.pack_into('<I',image,section+36,0x20000000)
        marker = b'0.6.4-unlock-test mode=staged-unlock-test STAGED_UNLOCK_INTENT: UNLOCK_VERIFIED: booter_load_ad102.bin'
        image[512:512+len(marker)] = marker; self.image = bytes(image)
        profile = parse_capture(*fixture())
        profile = bind_restoration_report(profile,restoration_fixture(profile))
        profile = bind_rehearsal_success(profile,success_fixture(profile),True)
        self.generated = {'board_profile.h':board_header(profile['board']).encode(),
                          'rehearsal_profile.h':staged_header(profile).encode()}
        names = SOURCES | HOST_REQUIRED_SOURCES | {
            'windows/build.py','windows/build_staged.py','windows/board_profile.py',
            'windows/rehearsal_profile.py','windows/staged_profile.py',
            'windows/package_staged.py','windows/validate_staged.py','windows/README.md',
            'windows/tests/test_fixture.py','windows/src/staged_main.inc',
            'windows/src/staged_state.h','windows/deps/edk2/test_header.h',
            'kernel/gpu_unlock/test_header.h','windows/usb-staged/Update-USB.ps1',
            'windows/usb-staged/StagedUsb.psm1','windows/usb-staged/START-HERE.txt',
            'LICENSE','windows/THIRD_PARTY_NOTICES.md'}
        names |= {'windows/usb-staged/'+name for name in USB_FILES}
        names |= {'windows/benchmark/'+name for name in BENCHMARK_FILES}
        for name in names:
            path = self.root/name; path.parent.mkdir(parents=True,exist_ok=True)
            path.write_text('Synthetic input: '+name+'\n')
        self.result = dict(mode=MODE,version=VERSION,sha256=digest(self.image),size=len(self.image),
            hardware_tested=False,unlock_enabled=True,staged_unlock_enabled=True,helper_protocol=None,
            build_defines=copy.deepcopy(DEFINES),assets_verified=True,required_assets=copy.deepcopy(REQUIRED_ASSETS),
            compiled_sources=sorted(SOURCES),board_profile=copy.deepcopy(profile['board']),capture_profile=profile,
            generated_header_sha256={name:digest(data) for name,data in self.generated.items()},
            source_sha256=build_source_hashes(self.root))
        self.validation = dict(all_passed=True,mode=MODE,hardware_tested=False,windows_runtime_tested=False,
            real_transaction_verified=True,real_asset_io_verified=True,assets_verified=True,
            required_assets=copy.deepcopy(REQUIRED_ASSETS),image_sha256=digest(self.image),
            tested_source_sha256=validation_source_hashes(self.root),
            checks=[dict(name=name,exit_code=0) for name in sorted(EXPECTED_CHECKS)])
        self.vm = dict(passed=True,mode=MODE,image_sha256=digest(self.image),hardware_tested=False,
            windows_handoff_tested=False,secure_boot_tested=False,bitlocker_tested=False,unlock_tested=False,
            cases=[dict(name=name,passed=True,returned_to_firmware=True) for name in sorted(CASES)])

    def check(self):
        check_inputs(self.image,self.result,self.validation,self.vm,self.generated,self.root)

    def test_complete_consistent_staged_evidence_is_accepted(self): self.check()

    def test_mode_build_defines_and_explicit_unlock_enable_are_required(self):
        baseline = copy.deepcopy(self.result)
        changes = [('mode','firmware-rehearsal'),('mode','experimental-unlock'),('version','0.5.2-rehearsal'),
                   ('version','0.6.0-unlock-test'),('version','0.6.1-unlock-test'),('version','0.6.2-unlock-test'),('version','0.6.3-unlock-test'),
                   ('hardware_tested',True),('unlock_enabled',False),('staged_unlock_enabled',False),
                   ('helper_protocol','gu-session-v1'),('build_defines',{}),('assets_verified',False)]
        for key in DEFINES:
            defines = dict(DEFINES); defines[key] = 1-defines[key]; changes.append(('build_defines',defines))
        for key,value in changes:
            self.result = copy.deepcopy(baseline); self.result[key] = value
            with self.subTest(key=key,value=value),self.assertRaises(ValueError): self.check()

    def test_asset_pins_cannot_change_in_build_or_validation(self):
        for target in ('result','validation'):
            baseline = copy.deepcopy(getattr(self,target))
            for kind in ('missing','extra','bytes','sha256'):
                value = copy.deepcopy(baseline); pins = value['required_assets']
                first = next(iter(pins))
                if kind == 'missing': del pins[first]
                if kind == 'extra': pins['unexpected.bin'] = {'bytes':1,'sha256':'ab'*32}
                if kind == 'bytes': pins[first]['bytes'] += 1
                if kind == 'sha256': pins[first]['sha256'] = 'ab'*32
                setattr(self,target,value)
                with self.subTest(target=target,kind=kind),self.assertRaises(ValueError): self.check()
            setattr(self,target,baseline)

    def test_missing_or_skipped_critical_real_asset_tests_refuse_release(self):
        baseline = copy.deepcopy(self.validation)
        for key in ('real_transaction_verified','real_asset_io_verified','assets_verified'):
            for value in (False,None):
                self.validation = copy.deepcopy(baseline)
                if value is None: del self.validation[key]
                else: self.validation[key] = value
                with self.subTest(key=key,value=value),self.assertRaises(ValueError): self.check()

    def test_host_markers_require_both_executions_exactly_once(self):
        complete = REAL_TRANSACTION_MARKER+'\n'+REAL_ASSET_IO_MARKER+'\n'
        self.assertEqual(verify_critical_host_checks(complete),
                         {'real_transaction_verified':True,'real_asset_io_verified':True})
        for log in ('OK (skipped=2)',REAL_TRANSACTION_MARKER,REAL_ASSET_IO_MARKER,
                    complete+REAL_TRANSACTION_MARKER,complete+REAL_ASSET_IO_MARKER):
            with self.subTest(log=log),self.assertRaises(RuntimeError): verify_critical_host_checks(log)

    def test_prior_success_and_user_boot_confirmation_cannot_be_forged_by_flags(self):
        original = copy.deepcopy(self.result)
        mutations = [lambda p:p.update(unlock_authorized=True),lambda p:p.update(rehearsal_hardware_tested=True),
                     lambda p:p.pop('staged_unlock_evidence'),
                     lambda p:p['staged_unlock_evidence'].update(windows_boot_user_confirmed=False),
                     lambda p:p['staged_unlock_evidence'].update(restoration_verified=False),
                     lambda p:p['staged_unlock_evidence'].update(unlock_attempted=True),
                     lambda p:p['staged_unlock_evidence'].update(report_sha256='ab'*32),
                     lambda p:p['staged_unlock_evidence'].update(restoration_report_sha256='ab'*32),
                     lambda p:p['staged_unlock_evidence'].update(stage_edges=[1,2,3,4,5,6]),
                     lambda p:p['staged_unlock_evidence']['windows_preload'].update(sha256='ab'*32)]
        for mutate in mutations:
            self.result = copy.deepcopy(original); mutate(self.result['capture_profile'])
            with self.subTest(profile=self.result['capture_profile']),self.assertRaises(ValueError): self.check()

    def test_both_headers_must_regenerate_from_bound_evidence(self):
        original = copy.deepcopy(self.generated)
        for name in original:
            self.generated = copy.deepcopy(original); self.generated[name] += b'\n#define UNREVIEWED 1\n'
            self.result['generated_header_sha256'][name] = digest(self.generated[name])
            with self.subTest(name=name),self.assertRaisesRegex(ValueError,'profile headers'): self.check()
            self.result['generated_header_sha256'][name] = digest(original[name])
        self.generated = original|{'extra.h':b'unreviewed'}
        with self.assertRaises(ValueError): self.check()

    def test_compiled_source_set_cannot_include_legacy_or_omit_guards(self):
        original = self.result['compiled_sources']
        for changed in (original[:-1],original+[original[0]],original+['windows/src/session.c'],
                        [name for name in original if name != 'windows/src/staged_state.c'],
                        [name for name in original if name != 'windows/src/rehearsal_handoff.c']):
            self.result['compiled_sources'] = changed
            with self.subTest(sources=changed),self.assertRaisesRegex(ValueError,'source inventory'): self.check()

    def test_efi_hash_structure_and_required_mode_markers_are_bound(self):
        original = self.image
        for changed in (b'XX'+original[2:],original.replace(b'0.6.4-unlock-test',b'0.6.0-unlock-test'),
                        original.replace(b'0.6.4-unlock-test',b'0.6.1-unlock-test'),
                        original.replace(b'0.6.4-unlock-test',b'0.6.2-unlock-test'),
                        original.replace(b'0.6.4-unlock-test',b'0.6.3-unlock-test'),
                        original.replace(b'UNLOCK_VERIFIED:',b'UNRECOGNIZED___'),
                        original.replace(b'STAGED_UNLOCK_INTENT:',b'UNRECOGNIZED_ENTRY___')):
            self.image = changed; self.result.update(sha256=digest(changed),size=len(changed))
            with self.subTest(image=changed[:2]),self.assertRaises(ValueError): self.check()
        self.image = original+b'changed'
        self.result.update(sha256=digest(original),size=len(original))
        with self.assertRaisesRegex(ValueError,'EFI/build mismatch'): self.check()

    def test_legacy_entry_markers_cannot_be_added_to_staged_payload(self):
        original = self.image
        for marker in (b'DIAGNOSTIC_ONLY:',b'REHEARSAL_ONLY:',b'GUSSN001',b'EXPERIMENTAL WINDOWS PORT'):
            self.image = original+marker; self.result.update(sha256=digest(self.image),size=len(self.image))
            with self.subTest(marker=marker),self.assertRaisesRegex(ValueError,'legacy or alternate'): self.check()

    def test_build_sources_and_new_headers_cannot_change_after_build(self):
        for name in ('windows/src/staged_main.inc','windows/src/staged_state.c',
                     'windows/staged_profile.py','windows/build_staged.py'):
            path = self.root/name; old = path.read_bytes(); path.write_bytes(old+b'changed')
            with self.subTest(name=name),self.assertRaisesRegex(ValueError,'Build source'): self.check()
            path.write_bytes(old)
        added = self.root/'windows/src/late_header.h'; added.write_text('added after build')
        with self.assertRaisesRegex(ValueError,'Build source'): self.check()

    def test_missing_linked_and_external_build_provenance_is_refused(self):
        name = 'windows/src/staged_state.c'; path = self.root/name; old = path.read_bytes()
        path.unlink()
        with self.assertRaises(ValueError): self.check()
        other = self.root/'other.c'; other.write_bytes(old); path.symlink_to(other)
        with self.assertRaises(ValueError): self.check()
        path.unlink(); path.write_bytes(old)
        self.result['source_sha256']['../outside.c'] = 'ab'*32
        with self.assertRaises(ValueError): self.check()

    def test_validation_sources_include_new_updater_profiles_and_package(self):
        for name in ('windows/usb-staged/StagedUsb.psm1','windows/usb-staged/START-HERE.txt',
                     'windows/usb-staged/Benchmark-Stock.cmd','windows/usb-staged/Run-Benchmark.ps1',
                     'windows/benchmark/run_benchmark.py','windows/benchmark/kernels.ptx',
                     'windows/benchmark/README.md','windows/validate_staged.py',
                     'windows/package_staged.py','windows/tests/test_fixture.py'):
            path = self.root/name; old = path.read_bytes(); path.write_bytes(old+b'changed')
            with self.subTest(name=name),self.assertRaisesRegex(ValueError,'stale validation'): self.check()
            path.write_bytes(old)
        (self.root/'windows/tests/test_late.py').write_text('new check after validation')
        with self.assertRaisesRegex(ValueError,'stale validation'): self.check()

    def test_benchmark_inventory_rejects_missing_extra_or_linked_inputs(self):
        path = self.root/'windows/benchmark/kernels.ptx'; content = path.read_bytes()
        path.unlink()
        with self.assertRaisesRegex(ValueError,'benchmark source inventory'): self.check()
        other = self.root/'outside.ptx'; other.write_bytes(content); path.symlink_to(other)
        with self.assertRaisesRegex(ValueError,'linked staged validation source'): self.check()
        path.unlink(); path.write_bytes(content)
        extra = self.root/'windows/benchmark/REPORT.TXT'; extra.write_text('not a package input')
        with self.assertRaisesRegex(ValueError,'benchmark source inventory'): self.check()
        extra.unlink()
        cache = self.root/'windows/benchmark/__pycache__'; cache.mkdir()
        (cache/'run_benchmark.pyc').write_bytes(b'generated cache is excluded')
        self.check()

    def test_archive_includes_benchmark_runtime_launchers_and_sources_with_hashes(self):
        build_dir = self.root/'build'; build_dir.mkdir()
        (build_dir/'BOOTX64.EFI').write_bytes(self.image)
        (build_dir/'build-result.json').write_text(json.dumps(self.result))
        for name,data in self.generated.items(): (build_dir/name).write_bytes(data)
        validation_path = self.root/'VALIDATION.json'; validation_path.write_text(json.dumps(self.validation))
        vm_path = self.root/'VM-VALIDATION.json'; vm_path.write_text(json.dumps(self.vm))
        archive_path = self.root/'package.zip'
        def checked(image,result,validation,vm,generated):
            return check_inputs(image,result,validation,vm,generated,self.root)
        with patch.object(package_staged,'ROOT',self.root),patch.object(package_staged,'WIN',self.root/'windows'),\
             patch.object(package_staged,'check_inputs',checked):
            package_staged.build(archive_path,build_dir,validation_path,vm_path)
        with zipfile.ZipFile(archive_path) as archive:
            sums = json.loads(archive.read(NAME+'/SHA256SUMS.json'))
            for name in BENCHMARK_FILES:
                expected = (self.root/'windows/benchmark'/name).read_bytes()
                for member in ('benchmark/'+name,'source/windows/benchmark/'+name):
                    self.assertEqual(archive.read(NAME+'/'+member),expected)
                    self.assertEqual(sums[member],digest(expected))
            for name in USB_FILES:
                expected = (self.root/'windows/usb-staged'/name).read_bytes()
                for member in (name,'source/windows/usb-staged/'+name):
                    self.assertEqual(archive.read(NAME+'/'+member),expected)
                    self.assertEqual(sums[member],digest(expected))
            self.assertFalse(any(name.endswith(('.bin','.sys','.pyc')) for name in archive.namelist()))

    def test_named_validation_checks_and_overall_success_are_required(self):
        baseline = copy.deepcopy(self.validation)
        changes = [('all_passed',False),('mode','firmware-rehearsal'),('hardware_tested',True),
                   ('windows_runtime_tested',True),('image_sha256','ab'*32),('tested_source_sha256',{}),
                   ('checks',[]),('checks',baseline['checks'][:-1]),('checks',[baseline['checks'][0]]*4),
                   ('checks',[dict(c,exit_code=1 if i == 1 else 0) for i,c in enumerate(baseline['checks'])])]
        for key,value in changes:
            self.validation = copy.deepcopy(baseline); self.validation[key] = value
            with self.subTest(key=key),self.assertRaises(ValueError): self.check()

    def test_vm_requires_refusal_cases_and_cannot_claim_unlock_or_hardware(self):
        baseline = copy.deepcopy(self.vm)
        changes = [('passed',False),('mode','firmware-rehearsal'),('image_sha256','ab'*32),
                   ('cases',[]),('cases',baseline['cases'][:-1]),('cases',[baseline['cases'][0]]*3)]
        changes += [(key,True) for key in ('hardware_tested','windows_handoff_tested','secure_boot_tested',
                                          'bitlocker_tested','unlock_tested')]
        for key,value in changes:
            self.vm = copy.deepcopy(baseline); self.vm[key] = value
            with self.subTest(key=key),self.assertRaises(ValueError): self.check()
        for i in range(3):
            for key in ('passed','returned_to_firmware'):
                self.vm = copy.deepcopy(baseline); self.vm['cases'][i][key] = False
                with self.subTest(case=i,key=key),self.assertRaises(ValueError): self.check()

    def test_asset_verifier_rejects_missing_wrong_size_hash_and_link(self):
        assets = self.root/'assets'; assets.mkdir()
        with self.assertRaises(ValueError): verify_assets(assets)
        path = assets/'booter_load_ad102.bin'; path.write_bytes(b'wrong size')
        with self.assertRaises(ValueError): verify_assets(assets)
        path.write_bytes(bytes(REQUIRED_ASSETS[path.name]['bytes']))
        with self.assertRaisesRegex(ValueError,'SHA-256 mismatch'): verify_assets(assets)
        other = self.root/'asset.bin'; path.rename(other); path.symlink_to(other)
        with self.assertRaisesRegex(ValueError,'linked'): verify_assets(assets)

    def test_build_requires_explicit_user_confirmation_before_inputs_or_compiler(self):
        command = [sys.executable,str(WIN/'build_staged.py')]
        for name in ('profile','topology','restoration-report','rehearsal-report'):
            command += ['--'+name,'missing-synthetic.txt']
        p = subprocess.run(command,capture_output=True,text=True)
        self.assertNotEqual(p.returncode,0); self.assertIn('--windows-boot-confirmed',p.stderr)


if __name__ == '__main__': unittest.main()
