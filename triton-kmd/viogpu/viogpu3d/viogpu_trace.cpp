#include "viogpu_trace.h"

/* A single diagnostic session for the single display adapter supported by
 * this capture tool. Storage is append-only, never wraps, and is read only
 * after STOP. No allocation, file I/O or serial output occurs while recording. */
static KSPIN_LOCK traceLock;
static FAST_MUTEX controlLock;
static TRITON_TRACE_HEADER traceHeader;
static TRITON_TRACE_RECORD *traceRecords;
static volatile LONG64 commandSequence;
static KTIMER traceTimer;
static KDPC traceExpiryDpc;
static ULONGLONG traceDeadline;
static BOOLEAN traceExpired;
struct TraceThread { PETHREAD thread; VioGpuTraceTag tag; };
static TraceThread traceThreads[64];

static void TraceExpire(KDPC *, void *, void *, void *)
{
    KIRQL irql; KeAcquireSpinLock(&traceLock, &irql);
    if (traceHeader.enabled && KeQueryInterruptTime() >= traceDeadline) {
        traceHeader.enabled = 0;
        traceHeader.stop_ticks = KeQueryPerformanceCounter(NULL).QuadPart;
        traceHeader.complete = 0;
        traceExpired = TRUE;
    }
    KeReleaseSpinLock(&traceLock, irql);
}

void VioGpuTraceInitialize()
{
    KeInitializeSpinLock(&traceLock);
    ExInitializeFastMutex(&controlLock);
    KeInitializeTimer(&traceTimer);
    KeInitializeDpc(&traceExpiryDpc, TraceExpire, NULL);
}

ULONGLONG VioGpuTraceRun()
{
    return InterlockedCompareExchange(&traceHeader.enabled, 0, 0)
        ? (ULONGLONG)InterlockedCompareExchange64((volatile LONG64 *)&traceHeader.run, 0, 0) : 0;
}

ULONGLONG VioGpuTraceNextCommand()
{
    return VioGpuTraceRun() ? (ULONGLONG)InterlockedIncrement64(&commandSequence) : 0;
}

static void WriteLocked(ULONG kind, const VioGpuTraceTag &tag,
                         ULONGLONG a, ULONGLONG b, ULONGLONG c)
{
    if (!traceHeader.enabled || tag.run != traceHeader.run) return;
    if ((ULONG)traceHeader.count >= traceHeader.capacity) {
        ++traceHeader.dropped;
        return;
    }
    TRITON_TRACE_RECORD &r = traceRecords[traceHeader.count];
    r.seq = traceHeader.count;
    r.ticks = KeQueryPerformanceCounter(NULL).QuadPart;
    r.run = tag.run; r.frame = tag.frame; r.command = tag.command;
    r.arg0 = a; r.arg1 = b; r.arg2 = c;
    r.kind = kind; r.pid = tag.pid;
    r.tid = HandleToULong(PsGetCurrentThreadId()); r.context = tag.context;
    ++traceHeader.count;
}

void VioGpuTraceRecord(ULONG kind, const VioGpuTraceTag &tag,
                       ULONGLONG a, ULONGLONG b, ULONGLONG c)
{
    if (!tag.run || !VioGpuTraceRun()) return;
    KIRQL irql; KeAcquireSpinLock(&traceLock, &irql);
    WriteLocked(kind, tag, a, b, c);
    KeReleaseSpinLock(&traceLock, irql);
}

VioGpuTraceTag VioGpuTraceThreadTag()
{
    VioGpuTraceTag tag = {};
    if (!VioGpuTraceRun()) return tag;
    PETHREAD thread = PsGetCurrentThread();
    KIRQL irql; KeAcquireSpinLock(&traceLock, &irql);
    for (UINT i = 0; i < RTL_NUMBER_OF(traceThreads); ++i)
        if (traceThreads[i].thread == thread) { tag = traceThreads[i].tag; break; }
    if (!tag.run) tag.run = traceHeader.run;
    KeReleaseSpinLock(&traceLock, irql);
    return tag;
}

VioGpuTraceScope::VioGpuTraceScope(const VioGpuTraceTag &tag) : slot(-1)
{
    if (!tag.run || tag.run != VioGpuTraceRun()) return;
    PETHREAD thread = PsGetCurrentThread();
    KIRQL irql; KeAcquireSpinLock(&traceLock, &irql);
    for (UINT i = 0; i < RTL_NUMBER_OF(traceThreads); ++i)
        if (traceThreads[i].thread == thread) { slot = i; break; }
    if (slot < 0)
        for (UINT i = 0; i < RTL_NUMBER_OF(traceThreads); ++i)
            if (!traceThreads[i].thread) { slot = i; break; }
    if (slot >= 0) {
        previous = traceThreads[slot].tag;
        traceThreads[slot].thread = thread;
        traceThreads[slot].tag = tag;
    } else {
        ++traceHeader.dropped; // Correlation loss invalidates the capture.
    }
    KeReleaseSpinLock(&traceLock, irql);
}
VioGpuTraceScope::~VioGpuTraceScope()
{
    if (slot < 0) return;
    KIRQL irql; KeAcquireSpinLock(&traceLock, &irql);
    traceThreads[slot].tag = previous;
    if (!previous.run) traceThreads[slot].thread = NULL;
    KeReleaseSpinLock(&traceLock, irql);
}

void VioGpuTraceQueue(ULONG kind, ULONGLONG cookie, ULONG type, ULONG context,
                      ULONGLONG status)
{
    VioGpuTraceTag tag = VioGpuTraceThreadTag();
    if (!tag.context) tag.context = context;
    VioGpuTraceRecord(kind, tag, cookie, type, status);
}

NTSTATUS VioGpuTraceControl(TRITON_TRACE_REQUEST *r, ULONG size)
{
    if (!r || size < sizeof(*r) || r->type != TRITON_TRACE_ESCAPE_TYPE ||
        r->length != size - 4) return STATUS_INVALID_PARAMETER;
    if (r->operation != TT_STATUS &&
        !SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_DEBUG_PRIVILEGE), ExGetPreviousMode()))
        return STATUS_PRIVILEGE_NOT_HELD;
    ExAcquireFastMutex(&controlLock);
    NTSTATUS status = STATUS_SUCCESS;
    if (r->operation == TT_START) {
        if (!r->run || traceHeader.enabled) status = STATUS_DEVICE_BUSY;
        else if (!r->arg0 || r->arg0 > 90000) status = STATUS_INVALID_PARAMETER;
        else {
            SIZE_T bytes = sizeof(TRITON_TRACE_RECORD) * TRITON_TRACE_CAPACITY;
            TRITON_TRACE_RECORD *fresh = (TRITON_TRACE_RECORD *)
                ExAllocatePoolWithTag(NonPagedPool, bytes, 'rTiV');
            if (!fresh) status = STATUS_INSUFFICIENT_RESOURCES;
            else {
                if (traceRecords) ExFreePoolWithTag(traceRecords, 'rTiV');
                traceRecords = fresh;
                LARGE_INTEGER frequency; KeQueryPerformanceCounter(&frequency);
                KIRQL irql; KeAcquireSpinLock(&traceLock, &irql);
                RtlZeroMemory(&traceHeader, sizeof(traceHeader));
                traceHeader.magic = TRITON_TRACE_MAGIC;
                traceHeader.version = TRITON_TRACE_VERSION;
                traceHeader.record_size = sizeof(TRITON_TRACE_RECORD);
                traceHeader.run = r->run;
                traceHeader.frequency = frequency.QuadPart;
                traceHeader.capacity = TRITON_TRACE_CAPACITY;
                traceHeader.start_ticks = KeQueryPerformanceCounter(NULL).QuadPart;
                traceDeadline = KeQueryInterruptTime() + r->arg0 * 10000;
                traceExpired = FALSE;
                traceHeader.enabled = 1;
                KeReleaseSpinLock(&traceLock, irql);
                LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(r->arg0 * 10000);
                KeSetTimer(&traceTimer, due, &traceExpiryDpc);
            }
        }
    } else if (r->operation == TT_STOP) {
        if (r->run != traceHeader.run) status = STATUS_INVALID_PARAMETER;
        else {
            KIRQL irql; KeAcquireSpinLock(&traceLock, &irql);
            traceHeader.enabled = 0;
            traceHeader.stop_ticks = KeQueryPerformanceCounter(NULL).QuadPart;
            traceHeader.complete = !traceExpired;
            KeReleaseSpinLock(&traceLock, irql);
            KeCancelTimer(&traceTimer);
        }
    } else if (r->operation == TT_READ) {
        if (traceHeader.enabled || r->run != traceHeader.run || !traceRecords ||
            r->offset > (ULONG)traceHeader.count ||
            r->count > (size - sizeof(*r)) / sizeof(TRITON_TRACE_RECORD))
            status = STATUS_INVALID_PARAMETER;
        else {
            r->count = min(r->count, (ULONG)traceHeader.count - r->offset);
            RtlCopyMemory(r + 1, traceRecords + r->offset,
                          r->count * sizeof(TRITON_TRACE_RECORD));
        }
    } else if (r->operation != TT_STATUS) status = STATUS_NOT_SUPPORTED;
    KIRQL snapshotIrql; KeAcquireSpinLock(&traceLock, &snapshotIrql);
    r->header = traceHeader;
    KeReleaseSpinLock(&traceLock, snapshotIrql);
    ExReleaseFastMutex(&controlLock);
    return status;
}

void VioGpuTraceShutdown()
{
    KeCancelTimer(&traceTimer);
    KeRemoveQueueDpc(&traceExpiryDpc);
    KeFlushQueuedDpcs();
    ExAcquireFastMutex(&controlLock);
    KIRQL irql; KeAcquireSpinLock(&traceLock, &irql);
    traceHeader.enabled = 0;
    TRITON_TRACE_RECORD *records = traceRecords;
    traceRecords = NULL;
    KeReleaseSpinLock(&traceLock, irql);
    if (records) ExFreePoolWithTag(records, 'rTiV');
    ExReleaseFastMutex(&controlLock);
}
