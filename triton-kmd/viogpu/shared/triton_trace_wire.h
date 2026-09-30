/* Opt-in diagnostic ABI shared by native/WOW UMD, KMD and capture tools.
 * Fixed-width scalars only; no user or kernel pointers cross this interface. */
#ifndef TRITON_TRACE_WIRE_H
#define TRITON_TRACE_WIRE_H

#define TRITON_TRACE_MAGIC 0x3152435454495254ULL /* TRIT TCR1 */
#define TRITON_TRACE_VERSION 1u
#define TRITON_TRACE_MAPPING L"Global\\TritonVistaTraceV1"
#define TRITON_TRACE_CAPACITY 131072u
#define TRITON_TRACE_ESCAPE_TYPE 0x7000u

#if defined(_WIN32)
typedef long triton_trace_i32;
#else
typedef int triton_trace_i32;
#endif

enum TRITON_TRACE_OPERATION {
    TT_STATUS = 0, TT_START = 1, TT_STOP = 2, TT_READ = 3, TT_MARK = 4
};
enum TRITON_TRACE_KIND {
    TT_PRESENT_BEGIN = 1, TT_GPU_WAIT_BEGIN, TT_GPU_WAIT_END,
    TT_DRAIN_BEGIN, TT_DRAIN_END, TT_CALLBACK_BEGIN, TT_CALLBACK_END,
    TT_CONSUME_BEGIN, TT_CONSUME_END, TT_PRESENT_END,
    TT_GPU_SPAN, TT_GPU_UNAVAILABLE, TT_DEVICE, TT_BLT_BEGIN, TT_BLT_END,
    TT_KMD_MARK = 32, TT_COMMAND_CREATE, TT_COMMAND_SUBMIT, TT_COMMAND_RUN,
    TT_COMMAND_COMPLETE, TT_QUEUE_SEND, TT_QUEUE_RECV, TT_COPY_BEGIN,
    TT_COPY_END, TT_VBLANK, TT_SYNC_BEGIN, TT_SYNC_END, TT_VSYNC_TICK,
    TT_HOST_COMMAND_BEGIN = 64, TT_HOST_COMMAND_END, TT_HOST_FENCE,
    TT_HOST_SCANOUT, TT_HOST_COPY_BEGIN, TT_HOST_COPY_END,
    TT_HOST_UPLOAD_BEGIN, TT_HOST_UPLOAD_END, TT_HOST_DISPLAY_BEGIN,
    TT_HOST_DISPLAY_END
};

#pragma pack(push, 8)
typedef struct TRITON_TRACE_RECORD {
    unsigned long long seq;
    unsigned long long ticks;
    unsigned long long run;
    unsigned long long frame;
    unsigned long long command;
    unsigned long long arg0;
    unsigned long long arg1;
    unsigned long long arg2;
    unsigned int kind;
    unsigned int pid;
    unsigned int tid;
    unsigned int context;
} TRITON_TRACE_RECORD;

typedef struct TRITON_TRACE_HEADER {
    unsigned long long magic;
    unsigned int version;
    unsigned int record_size;
    unsigned long long run;
    unsigned long long frequency;
    unsigned int capacity;
    volatile triton_trace_i32 count;
    volatile triton_trace_i32 dropped;
    volatile triton_trace_i32 enabled;
    volatile triton_trace_i32 writers;
    unsigned int complete;
    unsigned long long start_ticks;
    unsigned long long stop_ticks;
    volatile long long next_frame;
    unsigned long long reserved[6];
} TRITON_TRACE_HEADER;

/* PrivateDriverDataSize includes trailing records for TT_READ. The leading
 * Type/DataLength retain the existing escape prefix. Count is input capacity
 * and output record count; Offset is a record index. */
typedef struct TRITON_TRACE_REQUEST {
    unsigned short type;
    unsigned short length;
    unsigned int operation;
    unsigned long long run;
    unsigned long long frame;
    unsigned long long arg0;
    unsigned long long arg1;
    unsigned int offset;
    unsigned int count;
    TRITON_TRACE_HEADER header;
} TRITON_TRACE_REQUEST;
#pragma pack(pop)

/* Stored in reserved[0], computed after stopping with that field zeroed.
 * Detects transfer corruption as well as an incomplete final write. */
static inline unsigned int triton_trace_crc32(unsigned int crc, const void *data,
                                             unsigned int length)
{
    const unsigned char *p = (const unsigned char *)data;
    crc = ~crc;
    while (length--) {
        crc ^= *p++;
        for (unsigned int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

#if defined(__cplusplus)
static_assert(sizeof(TRITON_TRACE_RECORD) == 80, "trace record ABI");
static_assert(sizeof(TRITON_TRACE_HEADER) == 128, "trace header ABI");
static_assert(sizeof(TRITON_TRACE_REQUEST) == 176, "trace request ABI");
#else
_Static_assert(sizeof(TRITON_TRACE_RECORD) == 80, "trace record ABI");
_Static_assert(sizeof(TRITON_TRACE_HEADER) == 128, "trace header ABI");
_Static_assert(sizeof(TRITON_TRACE_REQUEST) == 176, "trace request ABI");
#endif
#endif
