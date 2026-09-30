#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[6]
source = (Path(__file__).resolve().parents[2] / 'npt_device.c').read_text(encoding='utf-8-sig')
start = source.index('#if defined(NPT_D3D9_RUNTIME_DDI)', source.index('npt_device_destroy(struct npt_device *dev)\n{'))
end = source.index('\n}', source.index('npt_device_retain(struct npt_device *dev)', start)) + 2
implementation = source[start:end]
fixture = r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
// RUNTIME_DEFINE
typedef int mtx_t;
#define mtx_plain 0
#define mtx_init(p,t) ((void)(p),(void)(t),0)
#define mtx_lock(p) ((void)(p))
#define mtx_unlock(p) ((void)(p))
#define NPT_CALL_ONCE(state,call) do { if (!atomic_load(&(state))) { (void)(call); atomic_store(&(state),2); } } while(0)
struct npt_device { _Atomic unsigned runtime_refs; unsigned borrowed_runtime_device; };
static struct npt_device *g_npt_device;
static mtx_t g_npt_device_mutex;
static _Atomic int g_npt_device_mutex_inited;
static uint32_t g_npt_device_use_count;
static unsigned binding, live;
static struct npt_device *npt_device_create(void) {
 struct npt_device *d = calloc(1,sizeof(*d));
 d->borrowed_runtime_device = binding; ++live; return d;
}
static void npt_device_destroy(struct npt_device *d) { --live; free(d); }
// IMPLEMENTATION
int main(void) {
 binding = 100; struct npt_device *first = npt_device_acquire();
 binding = 200; struct npt_device *second = npt_device_acquire();
 printf("first binding=%u second binding=%u shared=%d\n", first->borrowed_runtime_device, second->borrowed_runtime_device, first==second);
 if (first==second || second->borrowed_runtime_device != 200) {
  fputs("FAIL: second D3D10 device reused the first runtime's transport\n",stderr); return 1;
 }
 npt_device_retain(second); npt_device_release(first);
 if (live != 1 || second->borrowed_runtime_device != 200) return 2;
 npt_device_release(second); npt_device_release(second);
 if (live) return 3;
 binding = 300; first = npt_device_acquire();
 if (!first || first->borrowed_runtime_device != 300 || live != 1) return 4;
 npt_device_release(first);
 if (live) return 5;
 puts("independent runtime transport lifetime and recreation passed"); return 0;
}
'''
for runtime in ('NPT_D3D10_RUNTIME_DDI', 'NPT_D3D9_RUNTIME_DDI'):
 for negative in (False, True):
  code = implementation
  if negative:
   # Recreate the original missing D3D10 ownership guards while keeping the
   # real acquire/release/retain implementation and adversarial fixture.
   code = code.replace(' || defined(' + runtime + ')', '') if runtime.endswith('10_RUNTIME_DDI') else code.replace('defined(NPT_D3D9_RUNTIME_DDI)', '0')
  with tempfile.TemporaryDirectory() as temporary:
   path=Path(temporary)/'test.c'
   path.write_text(fixture.replace('// IMPLEMENTATION',code).replace('// RUNTIME_DEFINE', '#define ' + runtime + ' 1'))
   subprocess.run(['clang','-std=c11','-fsanitize=address,undefined',str(path),'-o',str(path.with_suffix(''))],check=True)
   result=subprocess.run([str(path.with_suffix(''))],capture_output=True,text=True)
   if negative:
    assert result.returncode != 0 and 'FAIL:' in result.stderr, result
    print(runtime + ' singleton negative control passed')
   else:
    assert result.returncode == 0, result.stdout + result.stderr
    print(runtime + ': ' + result.stdout.strip())
