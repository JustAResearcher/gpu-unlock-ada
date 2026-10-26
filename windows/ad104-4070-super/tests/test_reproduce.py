#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host-only reproduction-tool tests; all payload bytes are synthetic."""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SUCCESS = '0x0000000000000000'
READ_ERROR = '0x8000000000000007'
REVIEWED_PROM = '46b0ff2e697582fccefde3cbcb62eaff423bdce76f853040445030a7c5cc81fb'
REVIEWED_CODE = {
    '0febfc8415226067e906adecb896ab45b03e969eac360a01ccfe81dd04c8a47e': (310, 20384),
    'ece28d4e20f28f5ca16bbe0a49ad6b0ad33956d55851c4ed620f617a6c237097': (315, 7712),
    '216c7270fa08ad9e4c3694d34d825e9e904554a1af5ffa2aeead4317c5537b37': (406, 138183),
}


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


prepare = load_module('prepare_reproduction', ROOT / 'prepare.py')
extract = load_module('extract_reproduction', ROOT / 'overlay/tools/extract_private_capture.py')


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def generated_bytes(size, seed):
    """Deterministic host data, never a captured firmware/code payload."""
    block = bytes((i + seed) % 256 for i in range(256))
    return (block * ((size + 255) // 256))[:size]


class PrepareTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.base, self.overlay = self.root / 'base', self.root / 'overlay'
        self.base.mkdir()
        self.overlay.mkdir()
        self.catalog = self.root / 'catalog.json'
        self.output = self.root / 'new-output'

    def pin(self, source, name, data):
        path = getattr(self, source) / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return {'source': source, 'sha256': sha256(data)}

    def assemble(self, files):
        self.catalog.write_text(json.dumps({'files': files}), encoding='utf-8')
        with contextlib.redirect_stdout(io.StringIO()):
            return prepare.prepare(self.base, self.overlay, self.catalog, self.output)

    def test_mix_copies_only_catalogued_exact_bytes(self):
        files = {
            'shared/header.h': self.pin('base', 'shared/header.h', b'base\x00\xff\r\n'),
            'override.c': self.pin('overlay', 'override.c', b'overlay\n'),
            'new/source.c': self.pin('overlay', 'new/source.c', b'new\x00source'),
        }
        self.pin('base', 'override.c', b'base copy must not win')
        self.pin('overlay', 'unlisted.c', b'not selected')
        result = self.assemble(files)
        self.assertEqual(Path(result), self.output)
        actual = {p.relative_to(self.output).as_posix(): p.read_bytes()
                  for p in self.output.rglob('*') if p.is_file()}
        self.assertEqual(actual, {'shared/header.h': b'base\x00\xff\r\n',
                                  'override.c': b'overlay\n', 'new/source.c': b'new\x00source'})

    def test_corrupt_base_or_overlay_refuses_before_output_creation(self):
        for source in ('base', 'overlay'):
            with self.subTest(source=source):
                files = {'first.c': self.pin('base', 'first.c', b'valid'),
                         'last.c': self.pin(source, 'last.c', b'reviewed')}
                (getattr(self, source) / 'last.c').write_bytes(b'changed')
                with self.assertRaises(ValueError):
                    self.assemble(files)
                self.assertFalse(self.output.exists())

    def test_existing_output_is_preserved(self):
        files = {'source.c': self.pin('base', 'source.c', b'verified')}
        self.output.mkdir()
        sentinel = self.output / 'source.c'
        sentinel.write_bytes(b'keep existing bytes')
        with self.assertRaises(FileExistsError):
            self.assemble(files)
        self.assertEqual(list(self.output.iterdir()), [sentinel])
        self.assertEqual(sentinel.read_bytes(), b'keep existing bytes')

    def test_unsafe_catalog_paths_are_rejected(self):
        for name in ('../escape.c', 'sub/../escape.c', '/absolute.c',
                     'C:/absolute.c', 'C:relative.c', 'source.c:stream',
                     'sub\\source.c', '..\\escape.c', '\\\\host\\share\\source.c'):
            with self.subTest(name=name):
                files = {name: {'source': 'base', 'sha256': sha256(b'unused')}}
                with self.assertRaises(ValueError):
                    self.assemble(files)
                self.assertFalse(self.output.exists())

    def test_source_symlink_escape_is_rejected(self):
        outside = self.root / 'outside'
        outside.mkdir()
        (outside / 'source.c').write_bytes(b'outside source roots')
        for source in ('base', 'overlay'):
            for directory_link in (False, True):
                with self.subTest(source=source, directory_link=directory_link):
                    link = getattr(self, source) / ('linked-dir' if directory_link else 'linked.c')
                    target = outside if directory_link else outside / 'source.c'
                    try:
                        link.symlink_to(target, target_is_directory=directory_link)
                    except (OSError, NotImplementedError) as error:
                        self.skipTest(f'Host cannot create symlinks: {type(error).__name__}')
                    name = 'linked-dir/source.c' if directory_link else 'linked.c'
                    files = {name: {'source': source, 'sha256': sha256(b'outside source roots')}}
                    with self.assertRaises(ValueError):
                        self.assemble(files)
                    self.assertFalse(self.output.exists())


class ExtractorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Match producer schema and real lengths, but never use private bytes.
        # Only digest pins change for host tests; fixture labels and sizes do not.
        cls.prom = b'\x55\xaa' + generated_bytes(1048574, 19)
        cls.drivers = {label: generated_bytes(size, seed)
                       for (label, size), seed in zip(REVIEWED_CODE.values(), (31, 53, 79))}
        cls.code_pins = {sha256(data): (label, len(data)) for label, data in cls.drivers.items()}
        config = bytearray(generated_bytes(4096, 7))
        config[:4] = bytes.fromhex('de108327')
        config[4:6] = b'\x03\x00'
        config[8] = 0xa1
        config[9:12] = b'\x00\x00\x03'
        config[14] = 0
        config[16:20] = bytes.fromhex('00000080')
        config[44:48] = bytes.fromhex('58143b41')
        cls.config = bytes(config)
        agents = []
        for index, (label, data) in enumerate(cls.drivers.items()):
            # Deliberately unrelated runtime handles: hash determines the label.
            agents.append(dict(agent=40 + index, image_handle=50 + index, source_handle=9,
                               binding_status=SUCCESS, binding_handle_matches=True,
                               image_status=SUCCESS, image_bytes=len(data) + 4096, code_type=3,
                               code=dict(rva='0x000002a0', length=len(data), relocations_in_code=0,
                                         loaded_sha256=sha256(data), hex=data.hex())))
        cls.template = dict(
            schema=5, probe_version='0.5.0-4070s-readonly-cold', probe_mode='readonly-cold-4070s',
            gpu_writes=False, unlock_attempted=False, hardware_support_validated=False,
            mmio_reads=True, pci_enumeration_complete=True, pci_enumeration_status=SUCCESS,
            target_id_count=1, exact_identity_count=1, unique_expected_identity_observed=True,
            display_devices=[dict(vendor='0x10de', device_id='0x2783', subsystem_vendor='0x1458',
                                  subsystem_device='0x413b', revision=0xa1, exact_expected_identity=True,
                                  config64=cls.config[:64].hex())],
            config256=cls.config[:256].hex(), config4096=cls.config.hex(), config4096_status=SUCCESS,
            physical_prom=dict(guard_passed=True, offset='0x00300000', length=len(cls.prom),
                               data_read_status=SUCCESS, identity_stable=True, complete=True,
                               read_attempts=259, sha256=sha256(cls.prom), hex=cls.prom.hex(),
                               unlock_eligibility=False),
            platform=dict(available=True, complete=True, atomic_snapshot=False,
                          release_authorized=False, enumeration_status=SUCCESS, gpu_agents=agents))

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.capture = self.root / 'capture.json'
        self.output = self.root / 'new-private'
        self.data = copy.deepcopy(self.template)

    def invoke(self, data=None, *, prom_pin=None, code_pins=None, total_size=None):
        raw = json.dumps(self.data if data is None else data).encode('utf-8')
        if total_size is not None:
            self.assertLessEqual(len(raw), total_size)
            raw += b' ' * (total_size - len(raw))
        self.capture.write_bytes(raw)
        with mock.patch.object(extract, 'PROM', sha256(self.prom) if prom_pin is None else prom_pin), \
                mock.patch.object(extract, 'CODE', self.code_pins if code_pins is None else code_pins), \
                mock.patch.object(sys, 'argv', ['extract_private_capture.py', str(self.capture),
                                               '--out', str(self.output)]), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            extract.main()

    def assert_refused(self, **kwargs):
        # Expected validation failures only; KeyError/TypeError remain test errors.
        with self.assertRaises((SystemExit, ValueError)) as rejected:
            self.invoke(**kwargs)
        if isinstance(rejected.exception, SystemExit):
            self.assertNotEqual(rejected.exception.code, 0)
        self.assertFalse(self.output.exists(), 'Rejected capture created its destination')

    def assert_expected_files(self):
        expected = {'physical-prom.bin': self.prom, 'pci-config4096.bin': self.config}
        expected.update({f'driver-{label}-code.bin': data for label, data in self.drivers.items()})
        self.assertEqual({p.name for p in self.output.iterdir()}, set(expected))
        for name, payload in expected.items():
            self.assertTrue((self.output / name).read_bytes() == payload, f'Wrong bytes in {name}')

    def test_reviewed_pins_match_the_4070_super_profile(self):
        self.assertEqual(extract.PROM, REVIEWED_PROM)
        self.assertEqual(extract.CODE, REVIEWED_CODE)

    def test_schema5_extracts_exact_bytes_using_hash_labels(self):
        self.data['platform']['gpu_agents'].reverse()
        self.invoke()
        self.assert_expected_files()

    def test_schema4_is_refused(self):
        self.data['schema'] = 4
        self.assert_refused()

    def test_wrong_probe_version_or_mode_is_refused(self):
        for key, value in (('probe_version', 'unreviewed'), ('probe_mode', 'not-cold')):
            with self.subTest(key=key):
                data = copy.deepcopy(self.template)
                data[key] = value
                self.assert_refused(data=data)

    def test_readonly_and_unique_identity_flags_are_required(self):
        for key, expected in (('gpu_writes', False), ('unlock_attempted', False),
                              ('pci_enumeration_complete', True),
                              ('unique_expected_identity_observed', True)):
            for value in (None, not expected):
                with self.subTest(key=key, value=value):
                    data = copy.deepcopy(self.template)
                    if value is None:
                        del data[key]
                    else:
                        data[key] = value
                    self.assert_refused(data=data)

    def test_wrong_or_ambiguous_gpu_identity_is_refused(self):
        for key, value in (('vendor', '0x1234'), ('device_id', '0x2705'),
                           ('subsystem_vendor', '0x1462'), ('subsystem_device', '0x0000'),
                           ('revision', 0xa2), ('exact_expected_identity', False)):
            with self.subTest(key=key):
                data = copy.deepcopy(self.template)
                data['display_devices'][0][key] = value
                self.assert_refused(data=data)
        self.data['display_devices'].append(copy.deepcopy(self.data['display_devices'][0]))
        self.assert_refused()

    def test_incomplete_platform_metadata_is_refused(self):
        for key in ('available', 'complete'):
            for value in (None, False):
                with self.subTest(key=key, value=value):
                    data = copy.deepcopy(self.template)
                    if value is None:
                        del data['platform'][key]
                    else:
                        data['platform'][key] = value
                    self.assert_refused(data=data)

    def test_missing_or_duplicate_driver_is_refused(self):
        for case in ('missing', 'duplicate'):
            with self.subTest(case=case):
                data = copy.deepcopy(self.template)
                agents = data['platform']['gpu_agents']
                if case == 'missing':
                    agents.pop()
                else:
                    agents.append(copy.deepcopy(agents[0]))
                self.assert_refused(data=data)

    def test_unknown_driver_hash_is_refused(self):
        code = self.data['platform']['gpu_agents'][0]['code']
        raw = generated_bytes(code['length'], 111)
        code.update(hex=raw.hex(), loaded_sha256=sha256(raw))
        self.assert_refused()

    def test_driver_payload_and_claimed_hash_are_bound(self):
        for change in ('bytes', 'claim'):
            with self.subTest(change=change):
                data = copy.deepcopy(self.template)
                code = data['platform']['gpu_agents'][0]['code']
                if change == 'bytes':
                    raw = bytearray.fromhex(code['hex'])
                    raw[-1] ^= 1
                    code['hex'] = raw.hex()
                else:
                    code['loaded_sha256'] = '0' * 64
                self.assert_refused(data=data)

    def test_driver_length_metadata_is_required_and_exact(self):
        for value in (None, 0, 20383):
            with self.subTest(value=value):
                data = copy.deepcopy(self.template)
                code = data['platform']['gpu_agents'][0]['code']
                if value is None:
                    del code['length']
                else:
                    code['length'] = value
                self.assert_refused(data=data)

    def test_truncated_driver_cannot_bypass_pinned_size(self):
        code = self.data['platform']['gpu_agents'][0]['code']
        raw = bytes.fromhex(code['hex'])[:-1]
        pins = dict(self.code_pins)
        label, size = pins.pop(code['loaded_sha256'])
        code.update(hex=raw.hex(), length=len(raw), loaded_sha256=sha256(raw))
        pins[sha256(raw)] = (label, size)
        self.assert_refused(code_pins=pins)

    def test_prom_completion_metadata_is_required_and_consistent(self):
        changes = (('complete', False), ('guard_passed', False), ('identity_stable', False),
                   ('length', 1048575), ('offset', '0x00300004'), ('read_attempts', 258),
                   ('data_read_status', READ_ERROR))
        for key, value in changes:
            for missing in (False, True):
                with self.subTest(key=key, missing=missing):
                    data = copy.deepcopy(self.template)
                    if missing:
                        del data['physical_prom'][key]
                    else:
                        data['physical_prom'][key] = value
                    self.assert_refused(data=data)

    def test_prom_payload_and_claimed_hash_are_bound(self):
        for change in ('bytes', 'claim', 'missing-claim'):
            with self.subTest(change=change):
                data = copy.deepcopy(self.template)
                prom = data['physical_prom']
                if change == 'bytes':
                    raw = bytearray(self.prom)
                    raw[-1] ^= 1
                    prom['hex'] = raw.hex()
                elif change == 'claim':
                    prom['sha256'] = '0' * 64
                else:
                    del prom['sha256']
                self.assert_refused(data=data)

    def test_truncated_prom_cannot_bypass_required_length(self):
        raw = self.prom[:-1]
        self.data['physical_prom'].update(hex=raw.hex(), length=len(raw), sha256=sha256(raw))
        self.assert_refused(prom_pin=sha256(raw))

    def test_config4096_is_required_and_complete(self):
        for value in (None, '', self.config[:-1].hex()):
            with self.subTest(length=None if value is None else len(value)):
                data = copy.deepcopy(self.template)
                data['config4096'] = value
                self.assert_refused(data=data)
        del self.data['config4096']
        self.assert_refused()

    def test_config4096_requires_success_status(self):
        for missing in (False, True):
            with self.subTest(missing=missing):
                data = copy.deepcopy(self.template)
                if missing:
                    del data['config4096_status']
                else:
                    data['config4096_status'] = READ_ERROR
                self.assert_refused(data=data)

    def test_config_identity_bytes_must_match_the_reviewed_gpu(self):
        for offset in (0, 2, 8, 44, 46):
            with self.subTest(offset=offset):
                data = copy.deepcopy(self.template)
                config = bytearray(self.config)
                config[offset] ^= 1
                data['config4096'] = config.hex()
                data['config256'] = config[:256].hex()
                data['display_devices'][0]['config64'] = config[:64].hex()
                self.assert_refused(data=data)

    def test_eight_mib_input_boundary_is_accepted(self):
        self.invoke(total_size=8 * 1024 * 1024)
        self.assert_expected_files()

    def test_input_over_eight_mib_is_refused(self):
        self.assert_refused(total_size=8 * 1024 * 1024 + 1)

    def test_existing_destination_is_preserved(self):
        self.output.mkdir()
        sentinel = self.output / 'physical-prom.bin'
        sentinel.write_bytes(b'keep existing bytes')
        with self.assertRaises((FileExistsError, SystemExit, ValueError)) as rejected:
            self.invoke()
        if isinstance(rejected.exception, SystemExit):
            self.assertNotEqual(rejected.exception.code, 0)
        self.assertEqual(list(self.output.iterdir()), [sentinel])
        self.assertEqual(sentinel.read_bytes(), b'keep existing bytes')


if __name__ == '__main__':
    unittest.main()
