# SPDX-License-Identifier: GPL-2.0-only
"""Actual staged adapter build against synthetic EFI ownership and core state."""
import os
import re
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_rehearsal import fixture_header

WIN = Path(__file__).resolve().parents[1]


class StagedAdapterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        root = Path(cls.temporary.name)
        (root/'rehearsal_profile.h').write_text(fixture_header())
        (root/'board_profile.h').write_text('''#pragma once
#define GU_PROFILE_VERSION 1
#define GU_PROFILE_ID 0x270510deU
#define GU_BOARD_SUBSYSTEM 0xe1301462U
#define GU_BOARD_CHIP 0x193000a1U
#define GU_PROFILE_SEGMENT 0U
#define GU_PROFILE_BUS 1U
#define GU_PROFILE_DEVICE 0U
#define GU_PROFILE_FUNCTION 0U
#define GU_PROFILE_BAR_BASE 0x83000000ULL
#define GU_PROFILE_BAR_SIZE 0x1000000ULL
#define GU_PROFILE_ROM_SIZE 150528U
#define GU_PROFILE_VRAM 16376U
#define GU_PROFILE_PROM_HASH ""
#define GU_PROFILE_ROM_HASH ""
#define GU_PROFILE_TAG "synthetic-staged-tests"
''')
        cls.executable = root/'staged-adapter'
        command = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-fshort-wchar',
                   '-DGU_STAGED_UNLOCK=1', '-DGU_ENABLE_UNLOCK=1', '-DGU_REHEARSAL=0',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   '-I'+str(root), '-I'+str(WIN/'src'),
                   '-I'+str(WIN/'deps/edk2'), '-I'+str(WIN/'deps/edk2/X64'),
                   str(WIN/'tests/test_staged_adapter.c')]
        command += [str(WIN/'src'/(name+'.c')) for name in ('rehearsal', 'capture_pe', 'sha256', 'checks')]
        result = subprocess.run(command+['-o', str(cls.executable)], capture_output=True, text=True)
        if result.returncode:
            cls.temporary.cleanup()
            raise AssertionError(result.stdout+result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_cases(self, cases):
        env = os.environ.copy()
        env['ASAN_OPTIONS'] = env.get('ASAN_OPTIONS', '')+':detect_leaks=0'
        env['UBSAN_OPTIONS'] = env.get('UBSAN_OPTIONS', '')+':halt_on_error=1'
        for case in cases:
            with self.subTest(case=case):
                result = subprocess.run([str(self.executable), case], capture_output=True,
                                        text=True, timeout=20, env=env)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                self.assertIn('staged adapter PASS', result.stdout)
                if case.startswith('status-'):
                    self.assert_status_clear(case, result.stderr)
                if case.startswith('evidence-'):
                    self.assert_evidence(case, result.stderr)

    def assert_status_clear(self, case, log):
        lines = re.findall(r'^REHEARSAL_PCI_STATUS_CLEAR_ACCEPTED (.+)$', log, re.MULTILINE)
        accepted = case.startswith(('status-clear-', 'status-final-'))
        self.assertEqual(len(lines), int(accepted), log)
        observations = re.findall(r'^REHEARSAL_PCI_DEVICE_STATUS (.+)$', log, re.MULTILINE)
        self.assertEqual(len(observations), 1, log)
        observed = {key: int(value, 0) for key, value in
                    re.findall(r'(\w+)=(0x[0-9a-f]+|[0-9]+)', observations[0])}
        before = 1 if case == 'status-clear-correctable' else 8 if case == 'status-clear-unsupported' else 9
        current = {'status-partial-correctable': 1, 'status-partial-unsupported': 8,
                   'status-uncleared': 9, 'status-new-nonfatal': 2,
                   'status-new-fatal': 4, 'status-pending': 32}.get(case, 0)
        offset = 0x82
        if case == 'status-stale-cap':
            offset, before, current = 0x8a, 0, 0
        self.assertEqual(observed, {'offset': offset, 'baseline': before, 'current': current,
                                   'xor': before ^ current, 'current_errors': current & 15,
                                   'pending': int(bool(current & 32))})
        if accepted:
            fields = {key: int(value, 0) for key, value in
                      re.findall(r'(\w+)=(0x[0-9a-f]+|[0-9]+)', lines[0])}
            expected = 1 if case == 'status-clear-correctable' else 8 if case == 'status-clear-unsupported' else 9
            self.assertEqual(fields, {'stage': 3, 'offset': 0x82, 'baseline': expected,
                                      'current': 0, 'cleared': expected, 'allowed_clear': 9})
            self.assertIn('REHEARSAL_PCI_CHECK stage=3 reason=verified status=0', log)
        else:
            self.assertIn('reason=raw-config', log)

    def assert_evidence(self, case, log):
        checks = re.findall(r'^REHEARSAL_PCI_CHECK (.+)$', log, re.MULTILINE)
        self.assertTrue(checks, log)
        post = [line for line in checks if 'post_flr=1' in line]
        self.assertEqual(len(post), 1, log)
        self.assertIn('stage=3', post[0])
        self.assertIn('first=0', post[0])
        name = case.removeprefix('evidence-')
        reasons = {
            'status-only': 'raw-config', 'control': 'raw-config',
            'bar-config': 'raw-config', 'vendor': 'identity',
            'subsystem': 'identity', 'cache': 'raw-config',
            'raw-attributes': 'raw-attributes', 'decode': 'decode', 'bme': 'bme',
            'interface-global': 'interface-pointer', 'interface-swap': 'interface-pointer',
            'interface-unreadable': 'interface-unreadable',
            'interface-null': 'interface-status', 'interface-warning': 'interface-status',
            'interface-error': 'interface-status', 'missing-callback': 'interface-callback',
            'table-change': 'interface-table', 'table-callback': 'interface-table',
            'location-warning': 'location-status', 'location-error': 'location-status',
            'read-warning': 'config-read', 'read-error': 'config-read',
            'attributes-warning': 'attributes-read', 'attributes-error': 'attributes-read',
            'bar-warning': 'bar-query', 'bar-error': 'bar-query',
            'bar-descriptor': 'bar-descriptor',
        }
        self.assertIn('reason='+reasons.get(name, 'verified')+' ', post[0])
        config_valid = not (name.startswith(('interface-', 'table-', 'location-', 'read-'))
                            or name == 'missing-callback')
        attributes_valid = config_valid and name not in (
            'vendor', 'subsystem', 'bme', 'attributes-warning', 'attributes-error')
        self.assertIn('config_valid='+str(int(config_valid)), post[0])
        self.assertIn('attributes_valid='+str(int(attributes_valid)), post[0])
        fields = dict(re.findall(r'(\w+)=([^ ]+)', post[0]))
        expected_status = 0 if name in ('success', 'silent-final', 'silent-final-refusal', 'log-failure') else 0x8000000000000006
        if name in ('vendor', 'subsystem', 'bme', 'bar-descriptor') or name.startswith(('location-', 'read-')):
            expected_status = 0x8000000000000003
        if name in ('attributes-warning', 'attributes-error', 'bar-warning', 'bar-error'):
            expected_status = 0x8000000000000007
        self.assertEqual(int(fields['status'], 0), expected_status)
        if name == 'log-failure':
            # The only marker that made it to disk records the pre-log verdict;
            # C separately requires the logging failure to block completion.
            self.assertNotIn('REHEARSAL_PCI_TRANSPORT ', log)
            return
        interface_failure = name.startswith(('interface-', 'table-')) or name == 'missing-callback'
        location_failure = name.startswith('location-')
        read_failure = name.startswith('read-')
        identity_failure = name in ('vendor', 'subsystem', 'bme')
        attrs_failure = name.startswith('attributes-')
        before_bar = (interface_failure or location_failure or read_failure or identity_failure or attrs_failure
                      or name in ('status-only', 'control', 'bar-config', 'cache', 'raw-attributes', 'decode'))
        called = {'interface': True, 'location': not interface_failure,
                  'config_read': not (interface_failure or location_failure),
                  'attributes': not (interface_failure or location_failure or read_failure or identity_failure),
                  'bar': not before_bar}
        transport = re.search(r'^REHEARSAL_PCI_TRANSPORT (.+)$', log, re.MULTILINE)
        self.assertIsNotNone(transport)
        raw_status = dict(re.findall(r'(\w+)=(0x[0-9a-f]+|[0-9]+)', transport[1]))
        for field, queried in called.items():
            expected = 0 if queried else 0x8000000000000013
            stem = 'read' if field == 'config_read' else field
            if name == stem+'-warning':
                expected = 1
            if name == stem+'-error':
                expected = 0x8000000000000007
            self.assertEqual(int(raw_status[field], 0), expected, (name, field, log))
        attr_line = re.search(r'^REHEARSAL_PCI_ATTRIBUTES (.+)$', log, re.MULTILINE)
        attr_fields = {key: int(value, 0) for key, value in
                       re.findall(r'(\w+)=(0x[0-9a-f]+|[0-9]+)', attr_line[1])}
        expected_attr = 0x300
        if name == 'raw-attributes':
            expected_attr ^= 0x80000000
        if name == 'decode':
            expected_attr &= ~0x200
        self.assertEqual(attr_fields, {'valid': int(attributes_valid), 'baseline': 0x300,
                                      'expected': 0x300, 'actual': expected_attr if attributes_valid else 0,
                                      'decode_expected': 0x300 if config_valid else 0, 'missing_normalized': 0})
        api_line = re.search(r'^REHEARSAL_PCI_INTERFACE (.+)$', log, re.MULTILINE)
        api_fields = dict(re.findall(r'(\w+)=(0x[0-9a-f]+|[0-9]+)', api_line[1]))
        if name in ('interface-warning', 'interface-error', 'interface-null', 'interface-unreadable'):
            self.assertEqual(api_fields['readable'], '0')
            self.assertEqual(api_fields['table_same'], '0')
            self.assertNotIn('REHEARSAL_PCI_API_DIFF ', log)
        elif not interface_failure:
            self.assertEqual(api_fields['readable'], '1')
            self.assertEqual(api_fields['table_same'], '1')
        if not config_valid:
            self.assertNotIn('REHEARSAL_PCI_DEVICE_STATUS ', log)
        if not attributes_valid:
            self.assertNotIn('dededede', log)
        snapshots = re.findall(r'^REHEARSAL_PCI_SNAPSHOT source=(baseline|current) offset=(0x[0-9a-f]+|0) values=([0-9a-f/]+)$',
                               log, re.MULTILINE)
        if name != 'log-failure':
            for source in ('baseline', 'current'):
                rows = [(int(offset, 16), values) for actual, offset, values in snapshots if actual == source]
                self.assertEqual(len(rows), 16 if source == 'baseline' or config_valid else 0, log)
                if rows:
                    self.assertEqual([offset for offset, _ in rows], list(range(0, 256, 16)))
                    self.assertTrue(all(len(values.split('/')) == 4 for _, values in rows))
                    expected_words = [0]*64
                    for index, value in ((0, 0x270510de), (1, 0x00100003), (2, 0x030000a1),
                                         (4, 0x83000000), (11, 0xe1301462), (13, 0x40),
                                         (16, 0x00038001), (30, 0x00090000), (32, 0x00020010), (33, 1 << 28)):
                        expected_words[index] = value
                    if source == 'current':
                        changed = {'status-only': (30, 0), 'control': (30, 0x00090001),
                                   'bar-config': (4, 0x82000000), 'vendor': (0, 0x270510df),
                                   'subsystem': (11, 0xe1301463), 'cache': (3, 0x10), 'bme': (1, 0x00100007)}
                        if name in changed:
                            index, value = changed[name]
                            expected_words[index] = value
                    self.assertEqual([int(word, 16) for _, values in rows for word in values.split('/')], expected_words)
            if not config_valid:
                self.assertNotIn('dededede', log)
        diffs = {
            'status-only': ('0x78', '0x90000', '0', '0x90000'),
            'control': ('0x78', '0x90000', '0x90001', '0x1'),
            'bar-config': ('0x10', '0x83000000', '0x82000000', '0x1000000'),
            'vendor': ('0', '0x270510de', '0x270510df', '0x1'),
            'subsystem': ('0x2c', '0xe1301462', '0xe1301463', '0x1'),
            'cache': ('0xc', '0', '0x10', '0x10'),
            'bme': ('0x4', '0x00100003', '0x00100007', '0x4'),
        }
        if name in diffs:
            lines = [dict(re.findall(r'(offset|expected|actual|xor)=((?:0x)?[0-9a-f]+)', line))
                     for line in log.splitlines() if line.startswith('REHEARSAL_PCI_DIFF ')]
            expected = dict(zip(('offset', 'expected', 'actual', 'xor'), map(lambda x: int(x, 0), diffs[name])))
            self.assertIn(expected, [{key: int(value, 16) for key, value in line.items()} for line in lines], log)
        if name.startswith('table-'):
            self.assertIn('REHEARSAL_PCI_API_DIFF field='+('RomSize' if name == 'table-change' else 'Pci.Write'), log)

    def test_report10_semantic_leaf_replacement_boundaries(self):
        self.run_cases(['usb-replace-preflight-six', 'usb-replace-preflight-reversed', 'usb-replace-preflight-partial', 'usb-replace-preflight-retirement', 'usb-replace-stage4-six', 'usb-replace-final-six', 'usb-replace-final-refuse', 'usb-replace-generation-two'])

    def test_report10_invalid_replacements_refuse_before_any_mutation(self):
        self.run_cases(['usb-replace-refuse-'+name for name in ['extra', 'duplicate', 'path', 'agent', 'attributes', 'count', 'wrong-host', 'reuse-child', 'reuse-other', 'reuse-idle', 'reuse-permuted', 'old-alias', 'protocol', 'active', 'image-device', 'image-parent', 'host-path', 'host-interface', 'host-owner', 'path-unreadable', 'interface-unreadable', 'descendant', 'alias']])

    def test_replacement_diagnostics_cannot_hide_protected_changes(self):
        self.run_cases(['usb-replace-log-keyboard', 'usb-replace-stage4-refuse'])

    def test_usb_retirement_at_report9_boundary_after_claim_closed(self):
        self.run_cases(['usb-retire', 'usb-retire-absent', 'usb-retire-final', 'usb-final-extra-missing', 'usb-multi-unrelated'])

    def test_usb_retirement_never_masks_other_deltas_or_live_consumers(self):
        self.run_cases(['usb-active-child', 'usb-storage-child', 'usb-keyboard-child', 'usb-loaded-image-child',
                        'usb-host-owner-lost', 'usb-host-interface-drift', 'usb-host-path-drift',
                        'usb-child-path-drift', 'usb-child-interface-drift', 'usb-child-protocol-added', 'usb-child-active-now',
                        'usb-extra-missing', 'usb-poststate-extra-missing', 'usb-log-extra-missing', 'usb-log-added-edge', 'usb-added-edge', 'usb-added-child', 'usb-agent', 'usb-count',
                        'usb-host-path-bad', 'usb-child-path-bad', 'usb-child-path-not-usb',
                        'usb-child-path-prefix', 'usb-child-path-multi-instance',
                        'usb-host-interface-unreadable', 'usb-child-path-unreadable',
                        'usb-child-protocol-extra', 'usb-ancestor-storage', 'usb-ancestor-keyboard',
                        'usb-ancestor-late', 'usb-ancestor-alias', 'usb-multi-descendant'])

    def test_success_preserves_transaction_counters_and_requires_final_state(self):
        self.run_cases(['success', 'success-passive-edge', 'late-poststate',
                        'success-no-decode-change', 'success-io-decode-missing',
                        'success-memory-decode-missing', 'success-silent-final'])

    def test_release_callback_failures_do_not_claim_or_restore(self):
        self.run_cases([f'release-{effect}-{stage}' for effect in ('error', 'partial')
                        for stage in (1, 2, 3)])

    def test_claim_only_allows_one_exact_self_exclusive_edge(self):
        self.run_cases(['claim-'+name for name in (
            'preexisting-foreign', 'preexisting-self', 'preexisting-bme',
            'open-error', 'open-warning', 'open-partial', 'wrong-interface',
            'edge-agent', 'edge-controller', 'edge-flags', 'edge-count',
            'edge-duplicate', 'edge-protocol', 'edge-missing',
            'attributes-error', 'attributes-warning', 'attributes-partial',
            'supported-error', 'supported-warning', 'supported-missing-bits',
            'supported-mutation')])

    def test_pre_flr_detached_guard_accepts_owned_live_dma_only_without_bme(self):
        self.run_cases(['detached-'+name for name in (
            'live-dma', 'bme', 'flr', 'fault', 'master-pending', 'foreign')])

    def test_claim_rechecks_target_at_notify_without_logging_or_implicit_disconnect(self):
        self.run_cases(['claim-tpl-'+name for name in (
            'race-owner', 'map-error', 'protocol-warning', 'null-protocol-array',
            'protocol-count', 'null-guid', 'unreadable-guid', 'duplicate-guid',
            'no-pci', 'zero-open-count')])

    def test_incomplete_or_uncontained_core_state_cannot_restore(self):
        self.run_cases(['complete-'+name for name in (
            'not-started', 'not-quiesced', 'not-saved', 'no-flr', 'extra-flr',
            'no-writes', 'dma-count', 'dma-list', 'fault', 'retained',
            'master-pending', 'master-attribute-error', 'master-hardware-error',
            'bme', 'config', 'extra-edge', 'missing-self',
            'count-drift', 'keyboard-drift', 'poststate', 'postclose-state',
            'close-error', 'close-warning', 'close-partial', 'close-noop')])

    def test_restore_callbacks_and_every_poststate_check_fail_closed(self):
        self.run_cases([f'restore-{effect}-{stage}' for effect in ('error', 'partial', 'supported')
                        for stage in (1, 2, 3)])
        self.run_cases(['restore-poststate-4', 'restore-poststate-5', 'restore-poststate-6',
                        'restore-extra-edge', 'restore-dma-count'])

    def test_containment_refresh_authenticates_original_pci_without_clearing_failures(self):
        self.run_cases(['refresh-'+name for name in (
            'premature', 'prepared-only', 'success-fault', 'success-bme',
            'success-graph-drift', 'success-stale-pointer',
            'interface-pointer', 'interface-unreadable', 'interface-null',
            'interface-warning', 'interface-error', 'table-change',
            'location-warning', 'location-error', 'wrong-bus',
            'read-warning', 'read-error', 'id', 'class', 'header', 'subsystem',
            'map-error', 'code-xp', 'code-unmapped',
            'late-pointer', 'late-table', 'late-code-xp')])

    def test_config_evidence_preserves_exact_guards_and_uses_only_existing_reads(self):
        self.run_cases(['evidence-'+name for name in (
            'success', 'silent-final', 'silent-final-refusal',
            'status-only', 'control', 'bar-config', 'vendor', 'subsystem', 'cache',
            'raw-attributes', 'decode', 'bme',
            'interface-global', 'interface-swap', 'interface-unreadable',
            'interface-null', 'interface-warning', 'interface-error',
            'missing-callback', 'table-change', 'table-callback',
            'location-warning', 'location-error', 'read-warning', 'read-error',
            'attributes-warning', 'attributes-error',
            'bar-warning', 'bar-error', 'bar-descriptor', 'log-failure')])

    def test_status_clear_exception_is_only_clean_post_flr_and_reassertion_blocks_final(self):
        self.run_cases(['status-'+name for name in (
            'clear-reported', 'clear-correctable', 'clear-unsupported',
            'partial-correctable', 'partial-unsupported', 'uncleared',
            'new-nonfatal', 'new-fatal', 'pending', 'control', 'other-dword',
            'stale-cap', 'stale-version', 'malformed-cap',
            'preclaim-clear', 'preflr-clear',
            'final-reassert', 'final-control', 'final-flr-drift')])


if __name__ == '__main__':
    unittest.main()
