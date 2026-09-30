#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "triton_trace_wire.h"
typedef int BOOL;
typedef int32_t LONG;
typedef uint32_t ULONG, UINT;
typedef uint64_t UINT64;
#define TRUE 1
#define FALSE 0
typedef struct {void *traceMap; UINT64 traceRun,traceFrame; UINT traceContext;} TRITON9_DEVICE;
static LONG InterlockedCompareExchange(volatile int *p, LONG value, LONG expected)
{ __atomic_compare_exchange_n(p,&expected,value,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return expected; }
static LONG InterlockedExchangeAdd(volatile int *p,LONG value)
{ return __atomic_fetch_add(p,value,__ATOMIC_SEQ_CST); }
static LONG InterlockedIncrement(volatile int *p)
{ return __atomic_add_fetch(p,1,__ATOMIC_SEQ_CST); }
static unsigned GetCurrentProcessId(void) {return 1;}
static unsigned GetCurrentThreadId(void) {return 2;}
/* SOURCE_UNDER_TEST */
static TRITON_TRACE_HEADER *header;
static int restarted;
static void *producer(void *unused)
{
    (void)unused;
    TRITON9_DEVICE device = {header,1,1,1};
    /* Keep the workload finite even on runners with fewer CPUs than writers. */
    for(unsigned i=0;i<500;i++)
        triton9TraceEventAt(&device,TT_PRESENT_BEGIN,1,0,0,0);
    while (!__atomic_load_n(&restarted,__ATOMIC_SEQ_CST))sched_yield();
    for(unsigned i=0;i<1000;i++)
        triton9TraceEventAt(&device,TT_PRESENT_BEGIN,1,0,0,0);
    return NULL;
}
int main(void)
{
    header=calloc(1,sizeof(*header)+TRITON_TRACE_CAPACITY*sizeof(TRITON_TRACE_RECORD));assert(header);
    header->capacity=TRITON_TRACE_CAPACITY;header->run=1;header->enabled=1;
    pthread_t threads[8];
    for(unsigned i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,producer,NULL));
    while (__atomic_load_n(&header->count,__ATOMIC_SEQ_CST)<1000)sched_yield();
    __atomic_fetch_and(&header->enabled,~1,__ATOMIC_SEQ_CST);
    while (__atomic_load_n(&header->enabled,__ATOMIC_SEQ_CST))sched_yield();
    LONG count=header->count;assert(count>=1000 && !header->dropped);
    TRITON_TRACE_RECORD *records=(void*)(header+1);
    for(LONG i=0;i<count;i++)assert(records[i].seq==(unsigned)i && records[i].run==1 && records[i].frame==1);
    /* Old producers continue running across a new capture's initialization.
     * A stale reference may acquire the new gate, but must not append to it. */
    header->count=0;header->dropped=0;header->run=2;
    __atomic_store_n(&header->enabled,1,__ATOMIC_SEQ_CST);
    __atomic_store_n(&restarted,1,__ATOMIC_SEQ_CST);
    for(unsigned i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));
    assert(header->count==0 && header->enabled==1 && header->writers==0);
    free(header);puts("TRITON TRACE BUFFER CONCURRENCY PASS");return 0;
}
