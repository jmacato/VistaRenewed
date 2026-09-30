#!/usr/bin/env python3
"""Capture and inspect buffered Vista UMD/KMD/QEMU traces. See docs/TRITON_TRACE.md."""
from __future__ import annotations

import argparse
import base64
from collections import Counter, defaultdict
import ctypes as C
import ctypes.util
from dataclasses import dataclass
import hashlib
import gzip
import json
from pathlib import Path
import re
import socket
import statistics
import struct
import subprocess
import sys
import time
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from vista_control import Client, JOB_ROOT, DEFAULT_SOCKET

MAGIC = 0x3152435454495254
HEADER = struct.Struct('<QIIQQIiiiiIQQq6Q')
RECORD = struct.Struct('<8Q4I')
FIELDS = 'seq ticks run frame command arg0 arg1 arg2 kind pid tid context'.split()
NAMES = dict(enumerate(['Present', 'GPU wait begin', 'GPU wait end', 'Drain begin',
    'Drain end', 'Callback begin', 'Callback end', 'Consume begin', 'Consume end',
    'Present end', 'GPU timestamp span', 'GPU unavailable', 'Device', 'Blt', 'Blt end'], 1))
NAMES.update(dict(enumerate(['KMD mark', 'Command create', 'Command submit', 'Command run',
    'Command complete', 'Queue send', 'Queue receive', 'Copy begin', 'Copy end',
    'Vblank', 'Sync begin', 'Sync end', 'Vsync tick'], 32)))
NAMES.update(dict(enumerate(['Host command begin', 'Host command end', 'Host fence',
    'Scanout publish', 'Host copy begin', 'Host copy end', 'Upload begin', 'Upload end',
    'Display begin', 'Display end'], 64)))

class InvalidTrace(ValueError):
    pass

@dataclass
class Trace:
    layer: str
    run: int
    frequency: int
    start: int
    stop: int
    events: list[dict]

def decode(data: bytes, layer: str) -> Trace:
    if len(data) < HEADER.size:
        raise InvalidTrace(f'{layer}: truncated header')
    (magic, version, size, run, freq, cap, count, dropped, enabled, writers,
     complete, start, stop, next_frame, crc, *reserved) = HEADER.unpack_from(data)
    if magic != MAGIC or version != 1 or size != RECORD.size or not run:
        raise InvalidTrace(f'{layer}: unsupported ABI or zero run')
    if not 0 < cap <= 131072 or count < 0 or count > cap or dropped:
        raise InvalidTrace(f'{layer}: event loss or invalid count (count={count}, dropped={dropped})')
    if enabled or writers or complete != 1 or not freq or stop < start:
        raise InvalidTrace(f'{layer}: capture not stopped cleanly')
    if len(data) != HEADER.size + count * RECORD.size:
        raise InvalidTrace(f'{layer}: truncated file or trailing bytes')
    check = bytearray(data); check[80:88] = bytes(8)
    if crc != zlib.crc32(check):
        raise InvalidTrace(f'{layer}: checksum mismatch')
    events = [dict(zip(FIELDS, RECORD.unpack_from(data, HEADER.size + i * RECORD.size))) for i in range(count)]
    last = {}
    for i, event in enumerate(events):
        if event['seq'] != i or event['run'] != run or event['kind'] not in NAMES:
            raise InvalidTrace(f'{layer}: invalid sequence, run or event kind at {i}')
        # UMD Present entry timestamps precede taking the append-buffer slot;
        # concurrent threads can reserve slots in a different order.
        key = (event['pid'], event['tid'])
        if not start <= event['ticks'] <= stop or event['ticks'] < last.get(key, 0):
            raise InvalidTrace(f'{layer}: clock/order violation at {i}')
        last[key] = event['ticks']
    return Trace(layer, run, freq, start, stop, events)

def quantiles(values):
    if not values:
        return {'n': 0}
    ordered = sorted(values)
    return {'n': len(values), 'median': statistics.median(values),
            'p95': ordered[min(len(ordered)-1, int((len(ordered)-1)*.95))], 'max': max(values)}

def correlate(guest: Trace, host: Trace):
    """Bound host-minus-guest offset using causally matched virtqueue trips.

    Never assume synchronized wall clocks or use the average round-trip as
    exact latency. Each host begin lies after SEND and before RECV. A constant
    offset is accepted only if ALL complete brackets intersect, allowing one
    tick of quantization on each clock. Clock drift that breaks this model
    invalidates cross-clock results; local-clock spans remain available.
    """
    sends, receives, begins = {}, {}, defaultdict(list)
    for e in guest.events:
        if e['kind'] not in (37, 38):
            continue
        target = sends if e['kind'] == 37 else receives
        cookie = e['arg0']
        if not cookie and e['kind'] == 38:
            continue  # Request was already in flight before START.
        if not cookie or cookie in target:
            raise InvalidTrace('transport: zero or duplicate queue cookie')
        target[cookie] = e
    for e in host.events:
        if e['kind'] == 64 and e['command']:
            begins[e['command']].append(e)
    low, high = float('-inf'), float('inf')
    matched = 0
    tolerance = 1e9 / guest.frequency + 1e9 / host.frequency
    for cookie, s in sends.items():
        r = receives.get(cookie)
        hs = begins.get(cookie)
        if not r or not hs:
            continue  # Capture boundary / cursor queue; coverage is reported.
        if r['ticks'] < s['ticks']:
            raise InvalidTrace('transport: receive precedes send')
        for h in hs:  # A suspended QEMU command may be retried under one cookie.
            if h['arg0'] != s['arg1']:
                raise InvalidTrace('transport: command type mismatch')
            hn = h['ticks'] * 1e9 / host.frequency
            low = max(low, hn - r['ticks'] * 1e9 / guest.frequency - tolerance)
            high = min(high, hn - s['ticks'] * 1e9 / guest.frequency + tolerance)
        matched += 1
    if matched < 8:
        raise InvalidTrace(f'clocks: too few matched transport brackets ({matched})')
    if low > high:
        raise InvalidTrace(f'clocks: inconsistent offset bounds (gap {low-high:.0f} ns)')
    return {'offset_ns': (low+high)/2, 'uncertainty_ns': (high-low)/2,
            'matched': matched, 'send_count': len(sends), 'bounds_ns': [low, high]}, sends, receives, begins

def workload_errors(meta, stdout, kind):
    errors = []
    if meta.get('launch_error') or not meta.get('pid') or meta.get('end', 0) <= meta.get('begin', 0):
        errors.append('workload did not start and finish a measurable interval')
    samples = meta.get('samples', [])
    intervals = [(int(a),int(b)) for a,b in re.findall(r'begin=(\d+) end=(\d+) frequency=\d+', stdout)]
    # Initialization/readback and raster polling do not pump the application's
    # message queue. Judge responsiveness during the timed presentation phase.
    if kind == 'raster' and intervals:
        samples = [s for s in samples if any(a <= s[0] <= b for a,b in intervals)]
    elif kind == 'wmc':
        samples = [s for s in samples if s[0] >= meta.get('begin',0) + 5*meta.get('frequency',1)]
    active = [s for s in samples if s[2] and s[4]]
    if not active:
        errors.append('no visible window with the Triton UMD observed')
    elif any(s[1] != meta['pid'] for s in active):
        errors.append('workload lost foreground')
    elif not any(s[3] for s in active):
        errors.append('workload never answered responsiveness probes')
    if kind == 'raster':
        if meta.get('exit_code') != 0 or meta.get('timed_out'):
            errors.append('raster workload failed or timed out')
        if not re.search(r'RASTER-INTERVAL PASS .*bits=(32|64)', stdout):
            errors.append('missing raster PASS result')
        rates = re.findall(r'INTERVAL_(ONE|IMMEDIATE).*?rate=(\d+) frames=(\d+) seconds=([\d.]+) fps=([\d.]+) hr=([0-9a-fA-F]+) loaded=(\d+)', stdout)
        if len(rates) != 2 or {int(r[1]) for r in rates} != {60, 300}:
            errors.append('missing 60/300 Hz result pair')
        for r in rates:
            if int(r[2]) != 120 or int(r[5], 16) != 0 or r[6] != '1':
                errors.append('failed, occluded or unloaded raster Present')
        if 'mismatches=0' not in stdout or 'FAIL' in stdout:
            errors.append('RGB correctness not established')
    elif kind == 'wmc':
        if not meta.get('timed_out') or meta.get('exit_code') != 259:
            errors.append('WMC exited before the capture duration')
    else:
        errors.append('custom workload has no performance validity oracle')
    return errors

def wmc_caption(path):
    """Recognize only the reviewed English fullscreen 720p selected captions.

    This narrow workload oracle refuses unfamiliar layouts rather than treating
    any pixel change (clock, cursor, video, focus loss) as successful navigation.
    """
    from PIL import Image
    ref=json.loads((ROOT/'tests/fixtures/wmc-720p-captions.json').read_text())
    im=Image.open(path)
    if list(im.size)!=ref['viewport']: return {'label':None,'reason':'unsupported viewport'}
    roi=ref['roi']; width,height=roi[2]-roi[0],roi[3]-roi[1]
    actual={(i%width,i//width) for i,v in enumerate(im.convert('L').crop(roi).getdata()) if v>=ref['luma_threshold']}
    scores=[]
    for name,data in ref['templates'].items():
        expected={(i%width,i//width) for i,v in enumerate(zlib.decompress(base64.b64decode(data['mask_zlib_base64']))) if v}
        best=0.0
        for dy in range(-2,3):
            for dx in range(-2,3):
                shifted={(x+dx,y+dy) for x,y in actual if 0<=x+dx<width and 0<=y+dy<height}
                union=len(shifted|expected)
                best=max(best,len(shifted&expected)/union if union else 0)
        scores.append((best,name))
    scores.sort(reverse=True)
    return {'label':scores[0][1] if scores[0][0]>=.75 and scores[0][0]-scores[1][0]>=.25 else None,
            'score':scores[0][0],'runner_up':scores[1][0]}

def navigation_check(directory, manifest):
    observations=[]; errors=[]
    order=['Pictures + Videos','Music','TV + Movies','Online Media','Tasks']
    previous=None
    for nav in manifest.get('navigation',[]):
        try: found=wmc_caption(directory/nav['file'])
        except (OSError,ValueError): found={'label':None,'reason':'missing or unreadable image'}
        observations.append({**nav,**found})
        if found['label'] is None:
            errors.append('WMC selected caption could not be verified');previous=None;continue
        index=order.index(found['label'])
        if previous is not None:
            expected=max(0,min(len(order)-1,previous+(1 if nav['key']=='down' else -1)))
            if index != expected: errors.append('WMC selected caption did not follow the injected key')
        previous=index
    if len(observations)<4 or len({o['label'] for o in observations if o['label']})<3:
        errors.append('insufficient verified WMC navigation')
    return observations,sorted(set(errors))

def analyze(directory: Path):
    manifest = json.loads((directory / 'manifest.json').read_text())
    errors = []
    traces = {}
    required = {'workload.json','stdout.txt','environment.json'} | ({'umd.bin','kmd.bin','host.bin'} if manifest['tracing'] else set())
    if not required <= manifest['files'].keys():
        errors.append('integrity: required files are missing from the manifest')
    for name, digest in manifest['files'].items():
        if Path(name).name != name or hashlib.sha256((directory / name).read_bytes()).hexdigest() != digest:
            errors.append(f'integrity: {name} does not match manifest')
    try:
        meta = json.loads((directory / 'workload.json').read_text())
    except (OSError,ValueError):
        meta = {'pid':0}; errors.append('missing or malformed workload metadata')
    try:
        stdout = (directory / 'stdout.txt').read_text(errors='replace')
    except OSError:
        stdout = ''; errors.append('missing workload output')
    if manifest.get('capture_error'): errors.append('capture: '+manifest['capture_error'])
    if manifest.get('cleanup_error'): errors.append('cleanup: '+manifest['cleanup_error'])
    try:
        environment=json.loads((directory/'environment.json').read_text())
        binaries={name.casefold():digest for name,digest in environment['files'].items()}
        if not all(re.fullmatch('[0-9a-f]{64}',digest) for digest in binaries.values()):
            errors.append('guest binary fingerprint missing')
        if binaries.get(meta.get('loaded_module_path','').casefold()) != meta.get('loaded_module_file_sha256'):
            errors.append('observed UMD path/hash does not match the installed driver fingerprint')
        for name,digest in manifest.get('expected_package',{}).items():
            if name.lower().endswith(('.dll','.sys')) and not any(path.endswith('\\'+name.casefold()) and actual==digest for path,actual in binaries.items()):
                errors.append('installed binary differs from expected package: '+name)
    except (OSError,ValueError,KeyError):
        errors.append('missing or malformed guest binary metadata')
    errors += workload_errors(meta, stdout, manifest['kind'])
    if manifest.get('controller_exit') != 0:
        errors.append('capture controller failed')
    timeline = []
    report = {'schema': 1, 'run': manifest['run'], 'kind': manifest['kind'],
              'tracing': manifest['tracing'], 'physical_display_fps': None,
              'configured_mode_at_end': meta.get('mode'), 'errors': errors,
              'limitations': [
                  'QEMU display events end after eglSwapBuffers/glFlush; physical monitor delivery is not measured.',
                  'GPU query spans include elapsed GPU-clock time between Presents, including idle; they are not GPU busy time.',
                  'WMC shared-surface handoff to DWM is not assigned an invented causal frame link.',
                  'Clock alignment uses bounded transport observations; cross-clock durations inherit its uncertainty.']}
    report['responsiveness_probe_misses']=[{'guest_ticks':s[0],'foreground_pid':s[1]} for s in meta.get('samples',[]) if s[2] and not s[3]]
    report['responsiveness_probe_timeout_ms']=50
    if manifest['kind']=='wmc':
        report['navigation'], navigation_errors=navigation_check(directory,manifest)
        errors += navigation_errors
    if not manifest['tracing']:
        report['valid'] = not errors
        return report, timeline
    for layer in ('umd', 'kmd', 'host'):
        try:
            traces[layer] = decode((directory / (layer + '.bin')).read_bytes(), layer)
            if traces[layer].run != manifest['run']:
                raise InvalidTrace(f'{layer}: stale run')
        except (InvalidTrace, OSError) as exc:
            errors.append(str(exc))
    if len(traces) != 3:
        report['valid'] = False
        return report, timeline
    umd, kmd, host = (traces[x] for x in ('umd', 'kmd', 'host'))
    if umd.frequency != kmd.frequency:
        errors.append('guest UMD and KMD QPC frequencies differ')
    clock = None
    try:
        clock, sends, receives, begins = correlate(kmd, host)
        report['clock'] = clock
    except InvalidTrace as exc:
        errors.append(str(exc)); sends = receives = begins = {}

    frames = defaultdict(list)
    for e in umd.events:
        frames[e['frame']].append(e)
    target = {frame: events for frame, events in frames.items() if events[0]['pid'] == meta['pid']}
    if not target:
        errors.append('no UMD frames from the measured process')
    marks = defaultdict(list)
    for e in kmd.events:
        if e['kind'] == 32: marks[e['frame']].append(e)
    metrics = defaultdict(list)
    mode_metrics = defaultdict(lambda:defaultdict(list))
    mode_intervals = [(int(rate),int(begin),int(end)) for rate,begin,end in
        re.findall(r'INTERVAL_\w+[^\r\n]*rate=(\d+)[^\r\n]*begin=(\d+) end=(\d+)',stdout)]
    sends_by_frame = defaultdict(list)
    for s in sends.values(): sends_by_frame[s['frame']].append(s)
    completed_frames = {e['frame'] for e in kmd.events if e['kind']==36 and not (e['arg1'] & 0x80000000)}
    full, missing, failed, gpu, blts = 0, [], [], 0, 0
    published = {e['command'] for e in host.events if e['kind'] == 67}
    displayed = {e['command'] for e in host.events if e['kind'] == 73}
    scanout_frames, display_frames = set(), set()
    for cookie in published:
        if cookie in sends: scanout_frames.add(sends[cookie]['frame'])
    for cookie in displayed:
        if cookie in sends: display_frames.add(sends[cookie]['frame'])
    phase_pairs = [(1,10,'Present CPU'), (2,3,'GPU completion wait CPU'), (4,5,'Transport drain CPU'),
                   (6,7,'Runtime callback CPU'), (8,9,'Consumption wait CPU'), (14,15,'Windowed Blt CPU')]
    for frame, events in target.items():
        by_kind = defaultdict(list)
        for e in events: by_kind[e['kind']].append(e)
        start_kind = 1 if by_kind[1] else 14
        end_kind = 10 if start_kind == 1 else 15
        if len(by_kind[start_kind]) != 1 or len(by_kind[end_kind]) != 1:
            missing.append(frame); continue
        first = by_kind[start_kind][0]
        mode = next((str(rate)+' Hz' for rate,a,b in mode_intervals if a <= first['ticks'] <= b),'unclassified')
        if first['arg1'] != 0 or len(marks[frame]) != 1 or marks[frame][0]['pid'] != first['pid']:
            missing.append(frame); continue
        if by_kind[end_kind][0]['arg0'] != 0:
            failed.append(frame); continue
        if start_kind == 14:
            blts += 1
        else:
            queue = sends_by_frame[frame]
            if not queue or not all(s['arg0'] in receives and s['arg0'] in begins for s in queue):
                missing.append(frame); continue
            if frame not in completed_frames:
                missing.append(frame); continue
            full += 1
        for a,b,name in phase_pairs:
            if by_kind[a] and by_kind[b]:
                if len(by_kind[a]) != 1 or len(by_kind[b]) != 1 or by_kind[b][0]['ticks'] < by_kind[a][0]['ticks']:
                    errors.append(f'frame {frame}: invalid {name} pairing'); continue
                duration=(by_kind[b][0]['ticks'] - by_kind[a][0]['ticks']) * 1e6 / umd.frequency
                metrics[name].append(duration); mode_metrics[mode][name].append(duration)
        for e in by_kind[11]:
            if not e['arg2'] or e['arg1'] < e['arg0']:
                errors.append(f'frame {frame}: invalid GPU query result'); continue
            duration=(e['arg1'] - e['arg0']) * 1e6 / e['arg2']
            metrics['GPU timestamp interval'].append(duration)
            mode_metrics[mode]['GPU timestamp interval'].append(duration); gpu += 1
    report['target_frames'] = {'observed': len(target), 'correlated_presents': full, 'windowed_blts': blts,
                               'incomplete': missing, 'failed': failed, 'gpu_spans': gpu,
                               'scanout_published':len(scanout_frames & target.keys()),
                               'qemu_displayed_unique':len(display_frames & target.keys())}
    report['duration_us'] = {name: quantiles(values) for name, values in metrics.items()}
    report['duration_us_by_mode'] = {mode:{name:quantiles(v) for name,v in values.items()} for mode,values in mode_metrics.items()}
    # These durations stay entirely within one clock. Do not subtract a GPU
    # query interval from a CPU wait and call the difference driver overhead.
    pipeline_metrics = defaultdict(list)
    for trace, pairs in ((kmd,[(33,34,'KMD create to submit'),(34,35,'KMD scheduling'),(35,36,'KMD execution'),(42,43,'KMD vblank wait')]),
                          (host,[(64,65,'Host command CPU'),(68,69,'Host copy CPU'),(72,73,'QEMU display CPU')])):
        for a,b,label in pairs:
            pending = defaultdict(list)
            for e in trace.events:
                if e['kind']==a: pending[e['command']].append(e)
                elif e['kind']==b and pending[e['command']]:
                    s=pending[e['command']].pop(0)
                    if e['ticks'] < s['ticks']: errors.append(f'{label}: reversed event ordering')
                    else: pipeline_metrics[label].append((e['ticks']-s['ticks'])*1e6/trace.frequency)
    transport = defaultdict(list)
    for cookie,s in sends.items():
        if cookie in receives:
            transport[hex(s['arg1'])].append((receives[cookie]['ticks']-s['ticks'])*1e6/kmd.frequency)
    report['pipeline_duration_us'] = {name:quantiles(v) for name,v in pipeline_metrics.items()}
    report['pipeline_population'] = 'all captured work; application phases above are restricted to the measured PID'
    report['transport_round_trip_us_by_command_type'] = {name:quantiles(v) for name,v in transport.items()}
    if missing or failed:
        errors.append('target frames have missing correlation or failed operations')
    every=manifest.get('gpu_sample_every',1)
    report['gpu_sampling']={'every':every,'resolved':gpu,'coverage_fraction':gpu/full if full else None,
        'scope':'Each sample covers one consecutive Present interval; unsampled frames have no measured GPU duration.'}
    if manifest['kind'] == 'raster' and (full < 200 or gpu < (full-4)//every or gpu > (full+every-1)//every+4):
        errors.append('insufficient complete raster frames or resolved GPU timestamps')
    if full and (not gpu or gpu < max(1,(full-4)//every) or gpu > (full+every-1)//every+4):
        errors.append('resolved GPU timestamps do not meet the requested sampling coverage')
    if manifest['kind'] == 'raster' and (len(scanout_frames & target.keys()) < 200 or
                                       len(display_frames & target.keys()) < 8):
        errors.append('missing correlated scanout publication or QEMU display evidence')
    if manifest['kind'] == 'wmc' and (blts+full < 8 or len(manifest.get('navigation', [])) < 4):
        errors.append('insufficient WMC presentation frames or navigation evidence')
    report['event_counts'] = {layer: dict(Counter(NAMES[e['kind']] for e in trace.events)) for layer,trace in traces.items()}

    # Preserve every event, including unattributed work. GPU absolute clocks are
    # not calibrated; query durations remain arguments on the resolve event.
    origin_ns = umd.start * 1e9 / umd.frequency
    for layer, trace in traces.items():
        for e in trace.events:
            stamp = e['ticks'] * 1e9 / trace.frequency
            if layer == 'host':
                if not clock: continue
                stamp -= clock['offset_ns']
            args = {k: str(e[k]) for k in ('run','frame','command','arg0','arg1','arg2','context')}
            if e['kind'] == 11 and e['arg2']:
                args['gpu_interval_us'] = (e['arg1']-e['arg0'])*1e6/e['arg2']
            timeline.append({'name': NAMES[e['kind']], 'cat':layer, 'ph':'i', 's':'t',
                'ts':(stamp-origin_ns)/1000, 'pid':f'{layer}:{e["pid"]}', 'tid':e['tid'], 'args':args})
        # Paired spans on separate named tracks avoid falsely nesting independent
        # GPU/transport jobs that run on the same worker or overlap in flight.
        pairs = phase_pairs if layer == 'umd' else ([(35,36,'Command execution'),(42,43,'Vblank wait')] if layer == 'kmd' else [(64,65,'Host command'),(68,69,'Host copy'),(72,73,'QEMU display')])
        for a,b,name in pairs:
            pending = defaultdict(list)
            for e in trace.events:
                key = e['frame'] if layer == 'umd' else e['command']
                if e['kind'] == a: pending[key].append(e)
                elif e['kind'] == b and pending[key]:
                    s = pending[key].pop(0)
                    ts = s['ticks']*1e9/trace.frequency - origin_ns - (clock['offset_ns'] if layer == 'host' and clock else 0)
                    timeline.append({'name':name, 'cat':layer, 'ph':'X', 'ts':ts/1000,
                        'dur':(e['ticks']-s['ticks'])*1e6/trace.frequency,
                        'pid':layer, 'tid':f'{name}:{s["pid"]}:{s["tid"]}',
                        'args':{'frame':str(s['frame']), 'command':str(s['command'])}})
    flow_id=0
    if clock:
        for cookie,s in sends.items():
            if cookie not in begins: continue
            h=begins[cookie][0]; flow_id+=1
            for phase,layer,e,trace,offset in (('s','kmd',s,kmd,0),('f','host',h,host,clock['offset_ns'])):
                timeline.append({'name':'Virtqueue handoff', 'cat':'correlation', 'ph':phase,'id':flow_id,
                    'ts':(e['ticks']*1e9/trace.frequency-offset-origin_ns)/1000,
                    'pid':f'{layer}:{e["pid"]}','tid':e['tid'],
                    'args':{'cookie':str(cookie),'frame':str(s['frame']), 'clock_uncertainty_us':clock['uncertainty_ns']/1000}})
    report['valid'] = not errors
    return report, timeline

def chrome_timeline(events):
    """Chrome/Perfetto require numeric process and thread IDs, with names in M events."""
    pids={}; tids={}; result=[]
    for e in events:
        p=e['pid']; t=e['tid']; key=(p,t)
        if p not in pids:
            pids[p]=len(pids)+1
            result.append({'ph':'M','name':'process_name','pid':pids[p],'tid':0,'args':{'name':str(p)}})
        if key not in tids:
            tids[key]=len(tids)+1
            result.append({'ph':'M','name':'thread_name','pid':pids[p],'tid':tids[key],'args':{'name':str(t)}})
        result.append({**e,'pid':pids[p],'tid':tids[key]})
    return {'traceEvents':result,'displayTimeUnit':'ms'}

def export(directory):
    report, timeline = analyze(directory)
    (directory / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    with gzip.open(directory/'timeline.json.gz','wt',encoding='utf-8') as output:
        json.dump(chrome_timeline(timeline),output,separators=(',',':'))
    lines = [f"Capture {'VALID' if report['valid'] else 'INVALID'} — run {report['run']}", '',
             f"Physical display FPS: unmeasured. Configured mode at end: {report['configured_mode_at_end']}.", '']
    for error in report['errors']: lines.append(f'ERROR: {error}')
    if 'clock' in report:
        c = report['clock']; lines += [f"Clock alignment uncertainty: ±{c['uncertainty_ns']/1000:.3f} µs ({c['matched']} transport brackets).", '']
    for name, data in report.get('duration_us', {}).items():
        lines.append(f"{name}: median {data['median']:.2f} µs; p95 {data['p95']:.2f} µs; n={data['n']}.")
    for name, data in report.get('pipeline_duration_us', {}).items():
        lines.append(f"{name}: median {data['median']:.2f} µs; p95 {data['p95']:.2f} µs; n={data['n']}.")
    lines += ['', *report['limitations']]
    (directory / 'report.txt').write_text('\n'.join(lines) + '\n')
    print('\n'.join(lines[:18]))
    return 0 if report['valid'] else 1

class QMP:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX); self.socket.settimeout(10); self.socket.connect(str(path))
        self.stream = self.socket.makefile('rwb', buffering=0)
        json.loads(self.stream.readline()); self.command('qmp_capabilities')
    def command(self, name, args=None):
        self.stream.write((json.dumps({'execute':name, 'arguments':args or {}})+'\n').encode())
        while True:
            response = json.loads(self.stream.readline())
            if 'error' in response: raise RuntimeError(f'QMP {name}: {response["error"]}')
            if 'return' in response: return response['return']
    def close(self):
        self.stream.close(); self.socket.close()

def uncab(cab: Path, output: Path):
    """Extract exactly one regular CAB member through libarchive, never paths."""
    lib = C.CDLL(ctypes.util.find_library('archive'))
    lib.archive_read_new.restype = C.c_void_p
    for name in ('archive_read_support_format_cab','archive_read_free'):
        getattr(lib,name).argtypes = [C.c_void_p]
    lib.archive_read_open_filename.argtypes = [C.c_void_p,C.c_char_p,C.c_size_t]
    lib.archive_read_next_header.argtypes = [C.c_void_p,C.POINTER(C.c_void_p)]
    lib.archive_read_data.argtypes = [C.c_void_p,C.c_void_p,C.c_size_t]; lib.archive_read_data.restype = C.c_ssize_t
    ar = lib.archive_read_new()
    try:
        lib.archive_read_support_format_cab(ar)
        if lib.archive_read_open_filename(ar, bytes(cab), 65536) != 0: raise InvalidTrace('cannot open CAB')
        entry = C.c_void_p()
        if lib.archive_read_next_header(ar,C.byref(entry)) != 0: raise InvalidTrace('missing CAB entry')
        buf = C.create_string_buffer(65536); total = 0
        with output.open('xb') as out:
            while True:
                n = lib.archive_read_data(ar, buf, len(buf))
                if n < 0: raise InvalidTrace('corrupt CAB data')
                if not n: break
                total += n
                if total > HEADER.size + 131072 * RECORD.size: raise InvalidTrace('CAB exceeds trace capacity')
                out.write(buf.raw[:n])
        if lib.archive_read_next_header(ar,C.byref(entry)) != 1: raise InvalidTrace('extra CAB entries')
    finally:
        lib.archive_read_free(ar)

def capture(args):
    directory = args.output.resolve(); directory.mkdir(parents=True, exist_ok=False)
    run = uuid.uuid4().int & ((1<<63)-1) or 1
    remote = rf'C:\Windows\Temp\triton-trace-{run:x}'
    manifest = {'schema':1, 'run':run, 'kind':args.kind, 'command':args.command, 'seconds':args.seconds,
                'gpu_sample_every':args.gpu_every,
                'tracing':not args.off, 'files':{}, 'navigation':[], 'controller_exit':None,
                'host_time_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())}
    manifest['source_revision'] = subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    manifest['source_diff_sha256'] = hashlib.sha256(subprocess.check_output(['git','diff','--binary'],cwd=ROOT)).hexdigest()
    manifest['collector_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    if args.package_manifest:
        manifest['expected_package']=json.loads(args.package_manifest.read_text())
    source_paths=['tools/triton_trace.py','tools/triton_trace_guest.c',
        'triton-kmd/viogpu/shared/triton_trace_wire.h','triton-kmd/viogpu/viogpu3d/viogpu_trace.cpp',
        'triton-umd/src/virtio/neptune/vista-d3d9/triton9_trace.c','triton-qemu/ui/triton-trace.c']
    manifest['trace_source_sha256']={name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in source_paths}
    # /proc/PID/exe references the executable actually mapped by the running
    # process, even after a rebuild replaced its pathname on disk.
    state = json.loads(subprocess.check_output(['podman','inspect',args.container],text=True))[0]
    process = Path('/proc') / str(state['State']['Pid'])
    manifest['live_qemu_sha256'] = hashlib.sha256((process/'exe').read_bytes()).hexdigest()
    manifest['qemu_command'] = (process/'cmdline').read_bytes().replace(b'\0',b' ').decode(errors='replace')
    manifest['host_load_average'] = Path('/proc/loadavg').read_text().split()[:3]
    manifest['render_devices'] = []
    for node in Path('/sys/class/drm').glob('renderD*'):
        device=node/'device'
        manifest['render_devices'].append({'node':node.name, 'pci':device.resolve().name,
            'vendor':(device/'vendor').read_text().strip(),'device':(device/'device').read_text().strip(),
            'driver':(device/'driver').resolve().name})
    qmp = None; started = False; reply = None
    try:
        with Client(args.socket, timeout=120) as client:
            client.checked('WRITE', (remote+'.command.txt').encode()+b'\0'+args.command.encode('utf-8'))
            qmp = QMP(args.qmp)
            manifest['qemu'] = qmp.command('query-version')
            if not args.off:
                qmp.command('triton-trace-start', {'run':run}); started = True
            command = f'"{args.guest_exe}" {run:x} "{remote}" {args.seconds} "{remote}.command.txt" {0 if args.off else 1}'
            command += f' {args.deadline_ms or (args.seconds+15)*1000} {args.gpu_every}'
            reply = client.checked('USER', command.encode(), (args.seconds+30)*1000)
            deadline = time.monotonic()+args.seconds+45
            begin = time.monotonic(); next_key = begin+6; ordinal=0
            def workload_active():
                try:
                    data=client.checked('READ',(remote+'.active.json').encode())['data']
                    return json.loads(data).get('active') is True
                except (RuntimeError,ValueError):return False
            while reply['status'] == 'RUNNING':
                if time.monotonic() > deadline: raise RuntimeError('capture controller exceeded host deadline')
                now = time.monotonic()
                if args.kind == 'wmc' and now >= next_key and workload_active():
                    key = ('down','down','up','up')[ordinal%4]
                    sent = time.monotonic_ns()
                    qmp.command('send-key', {'keys':[{'type':'qcode','data':key}], 'hold-time':70})
                    # Screenshot and input share one scheduler. The next sample
                    # is taken after animation time, not in a competing process.
                    time.sleep(.4)
                    if not workload_active():
                        manifest['navigation_ended_during_key']=True
                        next_key=deadline
                        continue
                    path = directory / f'navigation-{ordinal:03d}.ppm'
                    capture_begin=time.monotonic_ns()
                    qmp.command('screendump', {'filename':str(path)})
                    capture_end=time.monotonic_ns()
                    from PIL import Image
                    png=path.with_suffix('.png');Image.open(path).save(png);path.unlink()
                    manifest['navigation'].append({'key':key,'sent_ns':sent,'capture_begin_ns':capture_begin,'capture_ns':capture_end,'file':png.name})
                    ordinal += 1; next_key = begin+6+ordinal*1.2
                time.sleep(.2)
                reply = client.checked('STATUS', job_id=reply['id'])
            manifest['controller_exit'] = reply['code'] if reply['status']=='DONE' else -1
            if started:
                manifest['host_stop'] = qmp.command('triton-trace-stop', {'path':str(directory/'host.bin')}); started=False
            with (directory/'controller.txt').open('wb') as f:
                client.read_file(JOB_ROOT+'\\'+reply['id']+'.out',f)
            with (directory/'controller-details.txt').open('wb') as f:
                client.read_file(remote+'.controller-details.txt',f)
            for suffix, name in (('.workload.json','workload.json'),('.stdout.txt','stdout.txt'),('.environment.json','environment.json')):
                with (directory/name).open('wb') as f: client.read_file(remote+suffix,f)
            if not args.off:
                for layer in ('umd','kmd'):
                    cmd = f'makecab /D CompressionType=LZX /D CompressionMemory=21 "{remote}.{layer}.bin" "{remote}.{layer}.cab"'
                    job=client.checked('RUN',cmd.encode(),120000)
                    while job['status']=='RUNNING':
                        time.sleep(.25); job=client.checked('STATUS',job_id=job['id'])
                    if job['code'] != 0: raise RuntimeError(f'{layer} compression failed')
                    with (directory/(layer+'.cab')).open('wb') as f: client.read_file(remote+'.'+layer+'.cab',f)
                    uncab(directory/(layer+'.cab'), directory/(layer+'.bin'))
    except BaseException as exc:
        manifest['capture_error'] = str(exc)
        print(f'capture failed: {exc}', file=sys.stderr)
    finally:
        if started and qmp:
            try: manifest['host_stop'] = qmp.command('triton-trace-stop', {'path':str(directory/'host.bin')})
            except Exception as exc: manifest['cleanup_error']=str(exc)
        if qmp: qmp.close()
        manifest['files'] = {p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in directory.iterdir() if p.is_file() and p.name!='manifest.json'}
        (directory/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return export(directory)

def overhead(args):
    if args.kind != 'raster': raise ValueError('overhead comparison currently requires the validated raster workload')
    root=args.output.resolve();root.mkdir(parents=True,exist_ok=False)
    runs=[]; deltas=defaultdict(list)
    for pair in range(args.pairs):
        values={}
        # Reverse each adjacent pair to expose rather than hide slow drift.
        for off in ((True,False) if pair%2==0 else (False,True)):
            label=f'{pair:02d}-'+('off' if off else 'on')
            child=argparse.Namespace(**vars(args));child.output=root/label;child.off=off
            print(f'Overhead run {label}',flush=True)
            valid=capture(child)==0
            text=(child.output/'stdout.txt').read_text(errors='replace')
            rates={int(rate):float(seconds)/int(frames)*1000 for rate,frames,seconds in
                re.findall(r'INTERVAL_\w+[^\r\n]*rate=(\d+) frames=(\d+) seconds=([\d.]+)',text)}
            runs.append({'directory':label,'tracing':not off,'valid':valid,'frame_ms':rates})
            values[off]=rates
            (root/'overhead.json').write_text(json.dumps({'complete':False,'runs':runs},indent=2)+'\n')
            if not valid or set(rates)!={60,300}: raise InvalidTrace(f'{label}: invalid overhead workload; comparison stopped')
        for rate in (60,300): deltas[rate].append(100*(values[False][rate]/values[True][rate]-1))
    summary={'complete':True,'runs':runs,'paired_frame_time_increase_percent':{rate:quantiles(v) for rate,v in deltas.items()},
             'interpretation':'Positive values mean tracing increased measured application frame time. Small samples show observed variation, not a confidence interval. Physical display FPS is unmeasured.'}
    (root/'overhead.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary['paired_frame_time_increase_percent'],indent=2));return 0

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='action',required=True)
    a=sub.add_parser('analyze'); a.add_argument('directory',type=Path)
    for action in ('capture','overhead'):
        c=sub.add_parser(action); c.add_argument('--output',type=Path,required=True)
        c.add_argument('--command',required=True); c.add_argument('--kind',choices=['raster','wmc','custom'],required=True)
        c.add_argument('--seconds',type=int,default=15); c.add_argument('--off',action='store_true')
        c.add_argument('--socket',type=Path,default=DEFAULT_SOCKET)
        c.add_argument('--qmp',type=Path,default=ROOT/'vista-kvm/x64-base/qmp.sock')
        c.add_argument('--guest-exe',default=r'C:\Windows\Temp\triton-trace-guest.exe')
        c.add_argument('--container',default='triton-vista-x64-normal')
        c.add_argument('--package-manifest',type=Path,help='Require installed DLL/SYS hashes to match this signed package manifest')
        c.add_argument('--deadline-ms',type=int,help='Override recorder watchdog for expiry testing (1..90000)')
        c.add_argument('--gpu-every',type=int,default=16,help='Sample one GPU interval per N Presents; 1 records every interval (default 16)')
        if action=='overhead':c.add_argument('--pairs',type=int,choices=range(2,11),default=4)
    args=parser.parse_args()
    if args.action=='analyze': return export(args.directory)
    if not 1<=args.seconds<=60: parser.error('--seconds must be 1..60')
    if args.deadline_ms is not None and not 1<=args.deadline_ms<=90000: parser.error('--deadline-ms must be 1..90000')
    if not 1<=args.gpu_every<=256: parser.error('--gpu-every must be 1..256')
    return overhead(args) if args.action=='overhead' else capture(args)

if __name__=='__main__':
    try: raise SystemExit(main())
    except (OSError,ValueError,RuntimeError) as exc:
        print(f'triton-trace: {exc}',file=sys.stderr); raise SystemExit(1)
