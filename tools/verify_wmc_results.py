#!/usr/bin/env python3
"""Validate recorded WMC acceptance evidence; never infer success from exit alone."""
import argparse
from pathlib import Path
import datetime as dt
import json
import re
import statistics

BASE = Path(__file__).resolve().parents[1] / 'vista-kvm/x64-base'

def read(name):
    return (BASE / name).read_text(errors='replace')

def intervals(name):
    times=[]
    for line in read(name).splitlines():
        if 'virtio_gpu_neptune_scanout_pixel ' in line:
            times.append(dt.datetime.fromisoformat(line.split()[0]).timestamp())
    assert len(times)>10, 'Insufficient scanout samples'
    gaps=sorted((b-a)*1000 for a,b in zip(times,times[1:]))
    assert gaps[0]>=0, 'Non-monotonic trace timestamps'
    return {'frames':len(times),'span_s':times[-1]-times[0],
            'median_ms':statistics.median(gaps),'p95_ms':gaps[int(.95*(len(gaps)-1))],
            'max_ms':max(gaps)}

def main():
    p=argparse.ArgumentParser();p.add_argument('check',choices=['baseline','admission','animation','clients','windowed']);a=p.parse_args()
    if a.check=='baseline':
        m=intervals('wmc-baseline-host.log')
        inputs=json.loads(read('wmc-baseline-input.json'))
        assert len(inputs)==32 and m['span_s']>20
        assert 'required=0 ' in read('wmc-caps-baseline.txt')
        modules=read('wmc-module-processes.txt')
        assert 'dwm.exe' in modules and 'ehshell.exe' not in modules
        assert m['median_ms']>500, 'Baseline must reproduce missing transition frames'
        print(json.dumps(m));print('WMC BASELINE VERIFIED')
    elif a.check=='admission':
        assert 'required=0 ' in read('wmc-caps-baseline.txt')
        assert 'hr=00000000 required=1 ' in read('audit29-wmc-caps.txt')
        assert re.search(r'ehshell\.exe\s+\d+\s+neptune_d3d9\.dll',read('audit38-modules.txt'))
        print('WMC HARDWARE ADMISSION VERIFIED')
    elif a.check=='animation':
        m=intervals('wmc-audit38-controlled-host.log');old=intervals('wmc-baseline-host.log')
        inputs=json.loads(read('wmc-audit38-controlled-input.json'))
        assert len(inputs)==32 and m['span_s']>20
        assert m['frames']>15*len(inputs), 'Too few transition frames'
        assert m['median_ms']<=20 and m['p95_ms']<=20 and m['max_ms']<=50, 'Animation timing still exceeds acceptance bounds'
        assert m['frames']>old['frames']*10
        # Timing alone can pass while WMC ignores all input. Require a
        # separately reviewed capture showing distinct selected menu rows.
        visual=json.loads(read('audit38-wmc-visual/review.json'))
        assert visual['navigation_verified'] is True
        assert len(set(visual['selected_rows']))>=3
        for name in visual['captures']: assert (BASE/'audit38-wmc-visual'/name).is_file()
        mode=re.search(r'DISPLAY width=(\d+) height=(\d+) refresh=(\d+) bits=(\d+)',read('audit38-current-mode.txt'))
        assert mode and tuple(map(int,mode.groups()))==(1280,720,300,32)
        m['configured_refresh_hz']=int(mode[3])
        m['configured_period_ms']=1000/int(mode[3])
        m['physical_display_fps_verified']=False
        print(json.dumps(m));print('WMC ANIMATION TIMING VERIFIED')
    elif a.check=='clients':
        for bits in (32,64):
            cases=[(f'audit38-raster{bits}-ex{ex}.txt', ex, 0, 0) for ex in (0,1)]
            cases += [(f'audit38-video{bits}.txt', 1, 1, 0)]
            cases += [(f'audit38-immediate{bits}-ex{ex}.txt', ex, 0, 1) for ex in (0,1)]
            for name,ex,video,immediate in cases:
                text=read(name)
                assert f'RASTER-INTERVAL PASS ex={ex} bits={bits} video={video} immediate={immediate}' in text, name
                assert len(re.findall(r'PIXELS .* checked=921600 mismatches=0',text))==6, name
                lines=re.findall(r'INTERVAL_(ONE|IMMEDIATE) ex=\d rate=(\d+) frames=120 seconds=[\d.]+ fps=([\d.]+) hr=00000000 loaded=1',text)
                assert len(lines)==2, name
                assert {int(rate) for _,rate,_ in lines}=={60,300}, name
                for kind,rate,fps in lines:
                    assert kind==('IMMEDIATE' if immediate else 'ONE'), name
                    if not immediate:
                        assert float(fps)<=int(rate)*1.2, name
                        if int(rate)==60: assert float(fps)>=50, name
                    elif int(rate)==60:
                        assert float(fps)>72, 'Immediate presentation was throttled: '+name
        print('D3D9 AND D3D9EX NATIVE AND WOW VERIFIED')
    else:
        for bits in (32,64):
            for ex in (0,1):
                name=f'audit38-windowed{bits}-ex{ex}.txt'
                text=read(name)
                assert f'DRIVER ex={ex} bits={bits} loaded=1' in text, name
                match=re.search(r'CADENCE fullscreen=0 frames=(\d+) seconds=([\d.]+) guest_fps=[\d.]+ occluded=0 hr=00000000',text)
                assert match and int(match[1])>=10 and float(match[2])>=5, name
        pixels=json.loads(read('audit38-windowed-visual/pixels.json'))
        visible=[p for p in pixels if p['palette_match'] and p['uniform_pixels']==p['checked'] and any(p['colour'])]
        assert len({tuple(p['colour']) for p in visible})>=4, 'Windowed output must visibly change'
        print('NATIVE AND WOW WINDOWED PRESENTATION VERIFIED')
if __name__=='__main__': main()
