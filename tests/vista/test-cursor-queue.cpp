#include <cassert>
#include <cstdint>
#include <cstdio>
#include <algorithm>
using UINT=unsigned;using ULONG=unsigned;using BOOLEAN=bool;using PVOID=void*;using PUCHAR=unsigned char*;using KIRQL=int;using LONG=long;
#define FALSE false
#define TRUE true
struct VirtIOBufferDescriptor{uintptr_t physAddr;unsigned length;};
struct GPU_VBUFFER{void *buf;unsigned size;void(*complete_cb)(void*,void*,void*);void *complete_ctx,*resp_buf;LONG complete_fired;};
using PGPU_VBUFFER=GPU_VBUFFER*;
LONG InterlockedExchange(LONG *p,LONG v){auto old=*p;*p=v;return old;}
static BOOLEAN BuildSGElement(VirtIOBufferDescriptor *s,PVOID p,ULONG n){if(!p||!n)return false;s->physAddr=(uintptr_t)p;s->length=std::min(n,4096u-unsigned((uintptr_t)p&4095));return true;}
class VioGpuQueue{public:int dropped=0,released=0;void ReleaseQueueBuffer(PGPU_VBUFFER){dropped++;}void ReleaseBuffer(PGPU_VBUFFER){released++;}};
class CrsrQueue:public VioGpuQueue{public:int result=0,kicks=0,adds=0;UINT count=0;VirtIOBufferDescriptor seen[2];void Lock(KIRQL*){}void Unlock(KIRQL){}void Kick(){kicks++;}int AddBuf(VirtIOBufferDescriptor *s,UINT n,int,PGPU_VBUFFER,void*,int){adds++;count=n;for(UINT i=0;i<n;i++)seen[i]=s[i];return result;}UINT QueueCursor(PGPU_VBUFFER);};
/* SOURCE_UNDER_TEST */
int main(){
 unsigned callbacks=0;GPU_VBUFFER b={(void*)uintptr_t(0x1ff0),56,[](void*p,void*,void*){++*(unsigned*)p;},&callbacks,nullptr,0};
 CrsrQueue q;assert(q.QueueCursor(&b)==0);assert(q.count==2&&q.seen[0].length==16&&q.seen[1].length==40);assert(q.seen[1].physAddr==0x2000);assert(q.kicks==1&&q.dropped==0);
 for(int error:{-1,-28}){CrsrQueue fail;fail.result=error;b.complete_fired=0;unsigned before=callbacks;assert(fail.QueueCursor(&b)==(UINT)-1);assert(fail.kicks==0&&fail.dropped==1&&callbacks==before+1);}
 CrsrQueue invalid;b.buf=nullptr;b.complete_fired=0;assert(invalid.QueueCursor(&b)==(UINT)-1);assert(invalid.adds==0&&invalid.dropped==1);
 assert(invalid.QueueCursor(nullptr)==(UINT)-1);
 puts("PASS cursor page split, queue-full/closed failure, callback and ownership release");
}
