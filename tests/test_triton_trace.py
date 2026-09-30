"""Independent, generated wire evidence: positive traces and destructive controls."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import triton_trace as tt

def record(ticks, kind, *, frame=0, command=0, a=0, b=0, c=0, pid=42):
    return dict(zip(tt.FIELDS, (0,ticks,123,frame,command,a,b,c,kind,pid,7,3)))

def wire(events, frequency=1000000, stop=10000000):
    events = sorted(events, key=lambda e:e['ticks'])
    data = bytearray(tt.HEADER.pack(tt.MAGIC,1,80,123,frequency,131072,len(events),0,0,0,1,0,stop,0,*([0]*6)))
    for i,e in enumerate(events):
        e = dict(e,seq=i)
        data.extend(tt.RECORD.pack(*(e[k] for k in tt.FIELDS)))
    struct.pack_into('<Q',data,80,zlib.crc32(data))
    return bytes(data)

def recalculate(data):
    data=bytearray(data); data[80:88]=bytes(8); struct.pack_into('<Q',data,80,zlib.crc32(data)); return bytes(data)

def positive(directory):
    umd=[]; kmd=[]; host=[]
    for frame in range(1,241):
        t=frame*10000
        for kind,offset in ((1,0),(2,10),(3,1010),(4,1020),(5,1070),(6,1080),(7,1130),(8,1140),(9,2140),(11,2150),(10,2160)):
            umd.append(record(t+offset,kind,frame=frame,a=100 if kind==11 else 0,b=200 if kind==11 else 0,c=1000000 if kind==11 else 64 if kind==1 else 0))
        kmd.extend([record(t+1,32,frame=frame),record(t+20,37,frame=frame,command=frame,a=frame,b=0x105),
                    record(t+1000,38,a=frame,b=0x105),record(t+1900,36,frame=frame,command=frame)])
        host.extend([record(t+520,64,command=frame,a=0x105),record(t+900,65,command=frame),
                     record(t+910,67,command=frame),record(t+920,72,command=frame),record(t+930,73,command=frame)])
    for layer,events in (('umd',umd),('kmd',kmd),('host',host)):
        (directory/(layer+'.bin')).write_bytes(wire(events))
    meta={'pid':42,'launch_error':0,'exit_code':0,'timed_out':False,'begin':1,'end':3000000,'frequency':1000000,
          'mode':[1280,720,300],'samples':[[100,42,1,1,1],[200,42,1,1,1]],
          'loaded_module_path':r'C:\Windows\System32\neptune_d3d9.dll','loaded_module_file_sha256':'a'*64}
    (directory/'workload.json').write_text(json.dumps(meta))
    (directory/'environment.json').write_text(json.dumps({'files':{meta['loaded_module_path']:'a'*64}}))
    (directory/'stdout.txt').write_text('PIXELS color=ff0000 checked=921600 mismatches=0\n'
        'INTERVAL_ONE ex=1 rate=60 frames=120 seconds=2.000000 fps=60.00 hr=00000000 loaded=1\n'
        'INTERVAL_ONE ex=1 rate=300 frames=120 seconds=1.000000 fps=120.00 hr=00000000 loaded=1\n'
        'RASTER-INTERVAL PASS ex=1 bits=64 video=0 immediate=0\n')
    manifest={'run':123,'kind':'raster','tracing':True,'controller_exit':0,'files':{}}
    (directory/'manifest.json').write_text(json.dumps(manifest)); reseal(directory)

def reseal(directory):
    p=directory/'manifest.json'; m=json.loads(p.read_text())
    m['files']={x.name:hashlib.sha256(x.read_bytes()).hexdigest() for x in directory.iterdir() if x.name!='manifest.json'}
    p.write_text(json.dumps(m))

class TracingTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.path=Path(self.tmp.name); positive(self.path)
    def report(self): return tt.analyze(self.path)[0]
    def test_positive_known_durations_and_clock_bound(self):
        report,timeline=tt.analyze(self.path)
        self.assertTrue(report['valid'],report['errors'])
        self.assertEqual(report['duration_us']['GPU completion wait CPU']['median'],1000)
        self.assertEqual(report['duration_us']['GPU timestamp interval']['median'],100)
        self.assertEqual(report['target_frames']['correlated_presents'],240)
        self.assertIsNone(report['physical_display_fps'])
        # The true synthetic offset is 500 us, bracket contains it; we cannot
        # infer the exact offset from a known 980 us guest round trip.
        self.assertLessEqual(report['clock']['bounds_ns'][0],500000)
        self.assertGreaterEqual(report['clock']['bounds_ns'][1],500000)
        self.assertTrue(any(e['ph']=='X' for e in timeline))
    def test_truncated_and_appended_files(self):
        data=(self.path/'umd.bin').read_bytes()
        for changed in (data[:50],data[:-1],data+b'x'):
            with self.assertRaises(tt.InvalidTrace): tt.decode(changed,'umd')
    def test_plausible_event_corruption_rejected_by_crc(self):
        data=bytearray((self.path/'umd.bin').read_bytes()); data[128+40]^=1
        with self.assertRaisesRegex(tt.InvalidTrace,'checksum'): tt.decode(data,'umd')
    def test_event_loss_and_incomplete_stop_rejected(self):
        original=(self.path/'umd.bin').read_bytes()
        for offset in (40,44,48,52):
            data=bytearray(original); struct.pack_into('<I',data,offset,1 if offset!=52 else 0)
            with self.assertRaises(tt.InvalidTrace): tt.decode(recalculate(data),'umd')
    def test_stale_run(self):
        p=self.path/'manifest.json'; m=json.loads(p.read_text()); m['run']=124;p.write_text(json.dumps(m))
        self.assertIn('umd: stale run',self.report()['errors'])
    def test_missing_frame_correlation(self):
        p=self.path/'kmd.bin'; trace=tt.decode(p.read_bytes(),'kmd')
        p.write_bytes(wire([e for e in trace.events if not(e['kind']==32 and e['frame']==9)])); reseal(self.path)
        self.assertFalse(self.report()['valid']); self.assertIn(9,self.report()['target_frames']['incomplete'])
    def test_duplicate_queue_identity(self):
        p=self.path/'kmd.bin'; trace=tt.decode(p.read_bytes(),'kmd')
        e=next(e.copy() for e in trace.events if e['kind']==37);e['ticks']+=1
        p.write_bytes(wire(trace.events+[e])); reseal(self.path)
        self.assertTrue(any('duplicate' in e for e in self.report()['errors']))
    def test_clock_jump_rejects_cross_clock_claim(self):
        p=self.path/'host.bin'; trace=tt.decode(p.read_bytes(),'host')
        for e in trace.events:
            if e['ticks']>1200000: e['ticks']+=5000
        p.write_bytes(wire(trace.events));reseal(self.path)
        self.assertTrue(any('inconsistent offset' in e for e in self.report()['errors']))
    def test_suspended_host_retry_within_bracket_is_valid(self):
        p=self.path/'host.bin';trace=tt.decode(p.read_bytes(),'host')
        e=next(e.copy() for e in trace.events if e['kind']==64);e['ticks']+=10
        p.write_bytes(wire(trace.events+[e]));reseal(self.path)
        self.assertTrue(self.report()['valid'])
    def test_no_gpu_timestamp_fails_full_performance_verdict(self):
        p=self.path/'umd.bin'; trace=tt.decode(p.read_bytes(),'umd')
        p.write_bytes(wire([e for e in trace.events if e['kind']!=11])); reseal(self.path)
        self.assertTrue(any('GPU timestamps' in e for e in self.report()['errors']))
    def test_occlusion_success_status_is_not_successful_present(self):
        p=self.path/'stdout.txt';p.write_text(p.read_text().replace('hr=00000000','hr=08760878')); reseal(self.path)
        self.assertFalse(self.report()['valid'])
    def test_wrong_foreground_hung_or_unloaded(self):
        p=self.path/'workload.json';original=json.loads(p.read_text())
        for col,value in ((1,99),(3,0),(4,0)):
            m=json.loads(json.dumps(original))
            for s in m['samples']:s[col]=value
            p.write_text(json.dumps(m)); reseal(self.path)
            self.assertFalse(self.report()['valid'])
    def test_failed_process_even_with_plausible_stdout(self):
        p=self.path/'workload.json';m=json.loads(p.read_text());m['exit_code']=1;p.write_text(json.dumps(m));reseal(self.path)
        self.assertFalse(self.report()['valid'])
    def test_manifest_integrity(self):
        p=self.path/'stdout.txt';p.write_text(p.read_text()+'edited')
        self.assertTrue(any('integrity:' in e for e in self.report()['errors']))
    def test_missing_required_hash(self):
        p=self.path/'manifest.json';m=json.loads(p.read_text());del m['files']['umd.bin'];p.write_text(json.dumps(m))
        self.assertFalse(self.report()['valid'])
    def test_wrong_installed_binary(self):
        p=self.path/'manifest.json';m=json.loads(p.read_text());m['expected_package']={'neptune_d3d9.dll':'b'*64};p.write_text(json.dumps(m))
        self.assertTrue(any('expected package' in e for e in self.report()['errors']))
    def test_no_scanout_or_display_rejected(self):
        for kind in (67,73):
            positive(self.path)
            p=self.path/'host.bin';t=tt.decode(p.read_bytes(),'host')
            p.write_bytes(wire([e for e in t.events if e['kind']!=kind]));reseal(self.path)
            self.assertFalse(self.report()['valid'])
    def test_numeric_viewer_ids_and_transport_flows(self):
        report,events=tt.analyze(self.path);self.assertTrue(report['valid'])
        trace=tt.chrome_timeline(events)
        self.assertTrue(all(isinstance(e['pid'],int) and isinstance(e['tid'],int) for e in trace['traceEvents']))
        self.assertEqual(sum(e['ph']=='s' for e in events),240)
        self.assertEqual(sum(e['ph']=='f' for e in events),240)

if __name__=='__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(TracingTests))
    if result.wasSuccessful(): print('TRITON_TRACE_VALIDATION_PASS')
    sys.exit(not result.wasSuccessful())
