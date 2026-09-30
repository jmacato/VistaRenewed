#pragma once
#include "helper.h"
#include "triton_trace_wire.h"

struct VioGpuTraceTag {
    ULONGLONG run, frame, command;
    ULONG context, pid;
};
void VioGpuTraceInitialize();
void VioGpuTraceShutdown();
NTSTATUS VioGpuTraceControl(TRITON_TRACE_REQUEST *request, ULONG size);
ULONGLONG VioGpuTraceRun();
ULONGLONG VioGpuTraceNextCommand();
void VioGpuTraceRecord(ULONG kind, const VioGpuTraceTag &tag,
    ULONGLONG a = 0, ULONGLONG b = 0, ULONGLONG c = 0);
void VioGpuTraceQueue(ULONG kind, ULONGLONG cookie, ULONG type, ULONG context,
                      ULONGLONG status = 0);
VioGpuTraceTag VioGpuTraceThreadTag();
class VioGpuTraceScope {
  public:
    explicit VioGpuTraceScope(const VioGpuTraceTag &tag);
    ~VioGpuTraceScope();
  private:
    LONG slot;
    VioGpuTraceTag previous;
};
