from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'triton-kmd/viogpu/viogpu3d/viogpu_vidpn.cpp').read_text();a=s.index('BOOLEAN VioGpuVidPN::TryPromoteFlip()');b=s.index('BOOLEAN VioGpuVidPN::TryPromoteFlipLocked()',a)
code=r'''
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cassert>
#include <cstdio>
using BOOLEAN=bool;using NTSTATUS=int;
static std::mutex state;static std::condition_variable cv;
static int attempts;static bool copying,release_copy;
static void ExAcquireFastMutex(std::mutex*m){{std::lock_guard<std::mutex>l(state);attempts++;cv.notify_all();}m->lock();}
static void ExReleaseFastMutex(std::mutex*m){m->unlock();}
class VioGpuVidPN {public:std::mutex m_flipSubmitMutex;NTSTATUS m_lastFlipStatus=0;bool armed=true;int outcome=0;
 BOOLEAN TryPromoteFlip();NTSTATUS CompletePendingFlip();
 BOOLEAN TryPromoteFlipLocked(){if(!armed)return false;armed=false;std::unique_lock<std::mutex>l(state);copying=true;cv.notify_all();cv.wait(l,[]{return release_copy;});m_lastFlipStatus=outcome;return outcome==0;}
};
'''+s[a:b]+r'''
int main(){
 for(int failure: {0,-5}) {
  VioGpuVidPN v;v.outcome=failure;attempts=0;copying=false;release_copy=false;std::atomic<bool>returned=false;int result=99;
  std::thread timer([&]{v.TryPromoteFlip();});
  {std::unique_lock<std::mutex>l(state);cv.wait(l,[]{return copying;});}
  std::thread dma([&]{result=v.CompletePendingFlip();returned=true;});
  {std::unique_lock<std::mutex>l(state);cv.wait(l,[]{return attempts==2;});assert(!returned);release_copy=true;cv.notify_all();}
  timer.join();dma.join();assert(returned&&result==failure);
 }
 VioGpuVidPN v;release_copy=true;assert(v.CompletePendingFlip()==0&&!v.armed);
 puts("PASS DMA waits for timer-owned copy; success/failure propagated; unclaimed flip copied before completion");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(code);exe=Path(d)/'test';subprocess.run(['clang++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
