#!/usr/bin/env python3
import socket,json,time,subprocess,datetime
from pathlib import Path
import argparse
p=argparse.ArgumentParser(description='Record 32 WMC navigation inputs and completed host scanouts; foreground WMC before running.')
p.add_argument('prefix');p.add_argument('--capture-dir')
p.add_argument('--profile-host', action='store_true', help='Record host fence notifications and copy-stage timings (host 39 or newer).')
args=p.parse_args()
b=Path(__file__).resolve().parents[1]/'vista-kvm/x64-base';s=socket.socket(socket.AF_UNIX);s.connect(str(b/'qmp.sock'));f=s.makefile('rwb',buffering=0);f.readline()
def q(n,a={}):
 f.write((json.dumps({'execute':n,'arguments':a})+'\n').encode())
 while True:
  r=json.loads(f.readline())
  if 'return' in r or 'error' in r:return r
q('qmp_capabilities')
captures=[]
if args.capture_dir:
 from PIL import Image
 capture_dir=b/args.capture_dir
 capture_dir.mkdir(parents=True,exist_ok=True)
start=datetime.datetime.now(datetime.timezone.utc).isoformat();events=[]
trace_events=['virtio_gpu_neptune_scanout_pixel']
if args.profile_host:
 trace_events += ['virtio_gpu_neptune_fence_wake', 'virtio_gpu_neptune_present_cost',
                  'virtio_gpu_fence_ctrl', 'virtio_gpu_fence_resp']
try:
 for event in trace_events:
  result=q('human-monitor-command',{'command-line':'trace-event '+event+' on'})
  assert result.get('return')=='', result
 for i in range(32):
  deadline=time.monotonic()+.75
  key='up' if i%8<4 else 'down'
  events.append({'t':time.time(),'key':key})
  result=q('human-monitor-command',{'command-line':'sendkey '+key})
  assert 'return' in result, result
  if args.capture_dir and i%3==2:
   time.sleep(.45)
   frame=capture_dir/'frame.ppm'
   result=q('screendump',{'filename':str(frame)})
   assert 'return' in result, result
   name=f'{len(captures):02}.png'
   Image.open(frame).save(capture_dir/name)
   captures.append({'file':name,'input_index':i,'t':time.time()})
   frame.unlink()
  time.sleep(max(0,deadline-time.monotonic()))
finally:
 for event in trace_events:
  q('human-monitor-command',{'command-line':'trace-event '+event+' off'})
 f.close();s.close()
r=subprocess.run(['podman','logs','--timestamps','--since',start,'triton-vista-x64-normal'],capture_output=True,text=True)
(b/(args.prefix+'-host.log')).write_text(r.stdout+r.stderr);(b/(args.prefix+'-input.json')).write_text(json.dumps(events,indent=2));print('captured',len(events),'navigation inputs')

if args.capture_dir: (capture_dir/'capture-times.json').write_text(json.dumps(captures,indent=2))
