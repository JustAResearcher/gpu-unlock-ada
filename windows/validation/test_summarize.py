# SPDX-License-Identifier: GPL-2.0-only
import copy
import json
import unittest
from summarize import FORMATS, summarize

def fixture(label, speed=1):
    result = dict(label=label, complete=True, hardware_settings_changed=False,
                  identity=['PRIVATE_DEVICE', 'PRIVATE_LOCATION', 'PRIVATE_DRIVER',
                            'PRIVATE_VBIOS', 'PRIVATE_MEMORY'], torch_version='test',
                  cuda_version='test', device_capability=[8, 9], script_sha256='a'*64,
                  utc='PRIVATE_TIMESTAMP', local_path='PRIVATE_PATH', results=[])
    for name in FORMATS:
        ms=2/speed
        result['results'].append(dict(name=name,n=4096,input_dtype='test',output_dtype='test',
            kernel='test',quantization_timed=False,reference='test',sample_count=1024,
            atol=.003,rtol=.003,repetitions=40,batches=3,ms_per_matmul=[ms,ms,ms],
            median_ms=ms,dense_tflops=2*4096**3/(ms*1e9),finite=True,correctness_passed=True,
            max_abs_error=.001,rms_error=.0001,telemetry={'uuid':'PRIVATE_UUID'}))
    return result

class SummaryTests(unittest.TestCase):
    def setUp(self):
        self.stock=fixture('restored');self.trial=fixture('trial',1.5)
    def test_numeric_only_allowlist(self):
        summary=summarize(self.stock,self.trial);text=json.dumps(summary)
        self.assertNotIn('PRIVATE_',text);self.assertNotIn('a'*64,text)
        self.assertFalse(summary['boot_or_unlock_success_inferred'])
        self.assertTrue(all(x['gain_percent']==50 for x in summary['rows']))
    def test_every_identity_and_case_difference_refused(self):
        from summarize import MATCH, CASE_MATCH
        for field in MATCH:
            with self.subTest(field=field):
                trial=copy.deepcopy(self.trial);trial[field]='different'
                with self.assertRaises((ValueError,TypeError)):summarize(self.stock,trial)
        for field in CASE_MATCH:
            with self.subTest(field=field):
                trial=copy.deepcopy(self.trial);trial['results'][0][field]=None
                with self.assertRaises((ValueError,TypeError)):summarize(self.stock,trial)
    def test_false_and_string_success_refused(self):
        for value in (False,'true',1,None):
            trial=copy.deepcopy(self.trial);trial['results'][0]['correctness_passed']=value
            with self.assertRaises(ValueError):summarize(self.stock,trial)
    def test_equal_but_absent_identity_refused(self):
        for field in ('identity', 'torch_version', 'cuda_version', 'device_capability'):
            with self.subTest(field=field):
                stock=copy.deepcopy(self.stock);trial=copy.deepcopy(self.trial)
                stock[field]=trial[field]=None
                with self.assertRaises((ValueError,TypeError)):summarize(stock,trial)
    def test_nonfinite_zero_negative_and_bool_timing_refused(self):
        for value in (float('nan'),float('inf'),0,-1,True):
            trial=copy.deepcopy(self.trial);trial['results'][0]['ms_per_matmul'][0]=value
            with self.assertRaises(ValueError):summarize(self.stock,trial)
    def test_misreported_median_or_rate_refused(self):
        for field in ('median_ms','dense_tflops'):
            trial=copy.deepcopy(self.trial);trial['results'][0][field]*=2
            with self.assertRaises(ValueError):summarize(self.stock,trial)
    def test_incomplete_formats_refused(self):
        trial=copy.deepcopy(self.trial);trial['results'].pop()
        with self.assertRaises(ValueError):summarize(self.stock,trial)
    def test_duplicate_formats_refused(self):
        trial=copy.deepcopy(self.trial);trial['results'][1]=trial['results'][0]
        with self.assertRaises(ValueError):summarize(self.stock,trial)
    def test_unchanged_or_slower_results_retained(self):
        rows=summarize(self.stock,fixture('trial',.9))['rows']
        self.assertTrue(all(x['gain_percent']==-10 for x in rows))
    def test_raw_injected_fields_not_copied(self):
        self.trial['results'][0]['PRIVATE_EXTRA']={'path':'PRIVATE_PATH'}
        self.assertNotIn('PRIVATE',json.dumps(summarize(self.stock,self.trial)))

if __name__=='__main__':unittest.main()
