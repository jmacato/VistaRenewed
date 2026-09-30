/* Production query creation and context dispatch with an ordered host model. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t HRESULT;
typedef uint32_t UINT;
typedef uint64_t UINT64;
typedef int BOOL, GUID, D3D11_QUERY;
typedef void ID3D11Asynchronous, ID3D11Query, ID3D11Query1;
typedef void ID3D11Predicate, ID3D11Counter;
typedef struct { D3D11_QUERY Query; UINT MiscFlags; } D3D11_QUERY_DESC;
typedef struct { D3D11_QUERY Query; UINT MiscFlags, ContextType; } D3D11_QUERY_DESC1;
typedef struct { UINT Counter, MiscFlags; } D3D11_COUNTER_DESC;
typedef struct { UINT64 Frequency; BOOL Disjoint; } D3D11_QUERY_DATA_TIMESTAMP_DISJOINT;
typedef struct { UINT64 values[11]; } D3D11_QUERY_DATA_PIPELINE_STATISTICS;
typedef struct { UINT64 written, needed; } D3D11_QUERY_DATA_SO_STATISTICS;
#define NPT_STDMETHODCALLTYPE
#define NPT_SUCCEEDED(hr) ((hr) >= 0)
#define NPT_S_OK 0
#define NPT_S_FALSE 1
#define NPT_PERF(flag) 0
#define NPT_QUERY_FEEDBACK_SLOT_RESULT 112
#define NPT_QUERY_FEEDBACK_SLOT_SIZE 128
#define NPT_QUERY_FEEDBACK_FLAG_READY 1

/* GENERATED_CONSTANTS */
struct npt_ring { int unused; } ring;
struct npt_device { struct npt_ring *ring; void *renderer; } device = {&ring, NULL};
struct npt_com_base {
    const void **lpVtbl;
    struct { struct npt_device *device; uint64_t id; } base;
    void *aux;
    void (*aux_destroy)(void *);
};
struct npt_query_feedback_slot { _Atomic uint64_t state; uint8_t result[112]; } slot;
struct npt_renderer_shmem { void *mmap_ptr; uint32_t res_id; } shmem = {&slot, 1};
/* AUX_SOURCE */

static unsigned allocations, registrations, unregistrations, unrefs;
static unsigned wire_calls, flags_seen, size_seen, creates;
static void *data_seen, *descriptor_seen;
static HRESULT create_status, host_status;
static uint64_t host_value;
static void (*init_aux)(struct npt_com_base *, struct npt_device *, uint64_t);
static size_t aux_size;
static struct npt_com_base query, context = {.base = {&device, 0x100}};
static struct npt_com_base device_wrapper = {.base = {&device, 0x200}};
static struct npt_device *npt_com_self_device(void *p)
{ return ((struct npt_com_base *)p)->base.device; }
static struct npt_ring *npt_com_self_ring(void *p)
{ return npt_com_self_device(p)->ring; }
static uint64_t npt_com_self_id(void *p)
{ return ((struct npt_com_base *)p)->base.id; }
static struct npt_renderer_shmem *npt_device_alloc_feedback_slot(
    struct npt_device *dev, uint32_t size, uint32_t *offset, bool *fresh)
{
    assert(dev == &device && size == 128);
    ++allocations; *offset = 0; *fresh = false; return &shmem;
}
static bool npt_ring_force_roundtrip(struct npt_ring *r) { assert(r == &ring); return true; }
static bool npt_dispatch_feedback_register_query(struct npt_ring *r, uint64_t id,
                                                uint32_t res, uint32_t off, uint32_t size)
{
    assert(r == &ring && id == query.base.id && res == 1 && !off && size <= 112);
    ++registrations; return true;
}
static void npt_dispatch_feedback_unregister_query(struct npt_ring *r, uint64_t id)
{ assert(r == &ring && id == query.base.id); ++unregistrations; }
static void npt_renderer_shmem_unref(void *renderer, struct npt_renderer_shmem *memory)
{ assert(!renderer && memory == &shmem); ++unrefs; }
static void npt_com_register_family(const GUID *const *tiers, size_t size,
                                  void (*init)(struct npt_com_base *, struct npt_device *, uint64_t))
{
    assert(tiers && tiers[0]);
    if (size) { aux_size = size; init_aux = init; }
}

static HRESULT create_query(void *self, const void *desc, void **out, const void *vtbl)
{
    assert(self == &device_wrapper); ++creates; descriptor_seen = (void *)desc;
    if (out) *out = NULL;
    if (create_status || !desc || !out) return create_status;
    assert(!query.aux && init_aux && aux_size);
    query.lpVtbl = (const void **)vtbl;
    query.base.device = &device; query.base.id = 0x300;
    query.aux = calloc(1, aux_size); assert(query.aux);
    init_aux(&query, &device, query.base.id); *out = &query;
    return 0;
}
static HRESULT npt_id3d11device_default_CreateQuery(void *self, const D3D11_QUERY_DESC *desc, ID3D11Query **out)
{ return create_query(self, desc, out, &npt_id3d11query_default_vtbl_storage); }
static HRESULT npt_id3d11device_default_CreatePredicate(void *self, const D3D11_QUERY_DESC *desc, ID3D11Predicate **out)
{ return create_query(self, desc, out, &npt_id3d11predicate_default_vtbl_storage); }
static HRESULT npt_id3d11device_default_CreateCounter(void *self, const D3D11_COUNTER_DESC *desc, ID3D11Counter **out)
{ return create_query(self, desc, out, &npt_id3d11counter_default_vtbl_storage); }
static HRESULT npt_id3d11device3_default_CreateQuery1(void *self, const D3D11_QUERY_DESC1 *desc, ID3D11Query1 **out)
{ return create_query(self, desc, out, &npt_id3d11query1_default_vtbl_storage); }

static char queued[16], observed[64];
static unsigned queued_count, observed_count;
static void issue(struct npt_ring *r, uint64_t id, ID3D11Asynchronous *async, char operation)
{
    assert(r == &ring && id == context.base.id && async == &query);
    assert(queued_count < sizeof(queued)); queued[queued_count++] = operation;
}
static void npt_async_ID3D11DeviceContext_Begin(struct npt_ring *r, uint64_t id, ID3D11Asynchronous *q)
{ issue(r, id, q, 'B'); }
static void npt_async_ID3D11DeviceContext_End(struct npt_ring *r, uint64_t id, ID3D11Asynchronous *q)
{ issue(r, id, q, 'E'); }
static HRESULT npt_call_ID3D11DeviceContext_GetData(struct npt_ring *r, uint64_t id,
                                                 ID3D11Asynchronous *async, void *data,
                                                 UINT size, UINT flags)
{
    assert(r == &ring && id == context.base.id && async == &query);
    ++wire_calls; flags_seen = flags; size_seen = size; data_seen = data;
    for (unsigned i = 0; i < queued_count; ++i) observed[observed_count++] = queued[i];
    queued_count = 0; observed[observed_count++] = 'G';
    if (host_status == 0 && data) {
        assert(size <= sizeof(host_value)); memcpy(data, &host_value, size);
    }
    return host_status;
}
/* DEFAULT_METHODS */

struct context_table {
    void (*Begin)(void *, ID3D11Asynchronous *);
    void (*End)(void *, ID3D11Asynchronous *);
    HRESULT (*GetData)(void *, ID3D11Asynchronous *, void *, UINT, UINT);
} contexts[5];
struct device_table {
    HRESULT (*CreateQuery)(void *, const D3D11_QUERY_DESC *, ID3D11Query **);
    HRESULT (*CreatePredicate)(void *, const D3D11_QUERY_DESC *, ID3D11Predicate **);
    HRESULT (*CreateCounter)(void *, const D3D11_COUNTER_DESC *, ID3D11Counter **);
    HRESULT (*CreateQuery1)(void *, const D3D11_QUERY_DESC1 *, ID3D11Query1 **);
} devices[6];
static void register_override(const char *iface, const char *method, void *function)
{
    if (!strncmp(iface, "id3d11devicecontext", 19)) {
        unsigned tier = iface[19] ? (unsigned)(iface[19] - '0') : 0;
        assert(tier < 5);
        if (!strcmp(method, "Begin")) contexts[tier].Begin = function;
        if (!strcmp(method, "End")) contexts[tier].End = function;
        if (!strcmp(method, "GetData")) contexts[tier].GetData = function;
    } else {
        assert(!strncmp(iface, "id3d11device", 12));
        unsigned tier = iface[12] ? (unsigned)(iface[12] - '0') : 0;
        assert(tier < 6);
        if (!strcmp(method, "CreateQuery")) devices[tier].CreateQuery = function;
        if (!strcmp(method, "CreatePredicate")) devices[tier].CreatePredicate = function;
        if (!strcmp(method, "CreateCounter")) devices[tier].CreateCounter = function;
        if (!strcmp(method, "CreateQuery1")) devices[tier].CreateQuery1 = function;
    }
}
#define NPT_REGISTER_OVERRIDE(iface, method, function) register_override(#iface, #method, (void *)(function))
static void ctx_Map_override(void) {}
static void ctx_Unmap_override(void) {}
static void ctx_UpdateSubresource_override(void) {}
static void ctx_UpdateSubresource1_override(void) {}
/* QUERY_SOURCE */
/* CONTEXT_SOURCE */
static void initialize_context_tables(void)
{
    /* DEFAULT_TABLES */
}
static void release_query(void)
{
    if (query.aux) query.aux_destroy(query.aux);
    query.aux = NULL;
}
static void reset_observation(void)
{
    allocations = registrations = unregistrations = unrefs = wire_calls = 0;
    queued_count = observed_count = 0; memset(observed, 0, sizeof(observed));
    host_status = NPT_S_FALSE; host_value = 0; atomic_store(&slot.state, 0);
}
static int failure(const char *message)
{ fprintf(stderr, "QUERY FAILURE: %s\n", message); return 1; }
int main(int argc, char **argv)
{
    bool end_only = argc == 2 && !strcmp(argv[1], "end-only");
    bool begin_end = argc == 2 && !strcmp(argv[1], "begin-end");
    D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP, 17};
    initialize_context_tables(); npt_overrides_d3d11_context_init(); npt_overrides_d3d11_query_init();
    for (unsigned tier = 0; tier < 6; ++tier) {
        void *out = NULL; reset_observation();
        assert(devices[tier].CreateQuery(&device_wrapper, &desc, &out) == 0 && out == &query);
        assert(descriptor_seen == &desc && npt_d3d11_query_aux_cast(out)->query_data_size == 8);
        if (!end_only && !begin_end && (allocations || registrations)) return failure("query registered shared feedback");
        release_query();
        D3D11_QUERY_DESC predicate = {D3D11_QUERY_OCCLUSION_PREDICATE, 0};
        reset_observation();
        assert(devices[tier].CreatePredicate(&device_wrapper, &predicate, &out) == 0);
        assert(npt_d3d11_query_aux_cast(out)->query_data_size == sizeof(BOOL));
        if (!end_only && !begin_end && (allocations || registrations)) return failure("predicate registered shared feedback");
        release_query();
        D3D11_COUNTER_DESC counter = {9, 3};
        assert(devices[tier].CreateCounter(&device_wrapper, &counter, &out) == 0);
        assert(descriptor_seen == &counter); release_query();
        if (tier >= 3) {
            D3D11_QUERY_DESC1 desc1 = {D3D11_QUERY_OCCLUSION, 4, 2}; reset_observation();
            assert(devices[tier].CreateQuery1(&device_wrapper, &desc1, &out) == 0);
            assert(descriptor_seen == &desc1 && npt_d3d11_query_aux_cast(out)->query_data_size == 8);
            if (!end_only && !begin_end && (allocations || registrations)) return failure("Query1 registered shared feedback");
            release_query();
        }
        create_status = (HRESULT)0x8007000e;
        assert(devices[tier].CreateQuery(&device_wrapper, &desc, &out) == create_status && !out);
        create_status = 0;
        assert(devices[tier].CreateQuery(&device_wrapper, NULL, &out) == 0 && !out);
        assert(devices[tier].CreateQuery(&device_wrapper, &desc, NULL) == 0);
    }
    for (unsigned tier = 0; tier < 5; ++tier) {
        void *out; reset_observation();
        assert(devices[0].CreateQuery(&device_wrapper, &desc, &out) == 0);
        struct npt_d3d11_query_aux *aux = query.aux;
        /* Poisoned legacy feedback must never bypass the ordered command. */
        aux->base.registered = true; aux->base.fb_shmem = &shmem;
        uint64_t stale = 100; memcpy(slot.result, &stale, 8); atomic_store(&slot.state, 1);
        if (!begin_end) {
            contexts[tier].End(&context, &query);
            uint64_t value = 0;
            if (contexts[tier].GetData(&context, &query, &value, 8, 1) != NPT_S_FALSE || value || wire_calls != 1)
                return failure("reissued END-only query returned stale completion");
            assert(!strcmp(observed, "EG") && flags_seen == 1 && size_seen == 8 && data_seen == &value);
            host_status = 0; host_value = 200;
            assert(contexts[tier].GetData(&context, &query, &value, 8, 0) == 0 && value == 200);
            assert(flags_seen == 0);
            host_status = NPT_S_FALSE; contexts[tier].End(&context, &query); value = 0;
            assert(contexts[tier].GetData(&context, &query, &value, 8, 1) == NPT_S_FALSE && !value);
            host_status = 0; host_value = 300;
            assert(contexts[tier].GetData(&context, &query, &value, 8, 1) == 0 && value == 300);
        }
        if (!end_only) {
            queued_count = observed_count = 0; memset(observed, 0, sizeof(observed));
            contexts[tier].Begin(&context, &query); contexts[tier].End(&context, &query);
            host_status = 0; host_value = 400;
            /* Legacy host poll always published generation zero. */
            memcpy(slot.result, &host_value, 8); atomic_store(&slot.state, 1);
            uint64_t value = 0;
            if (contexts[tier].GetData(&context, &query, &value, 8, 1) != 0 || value != 400)
                return failure("completed BEGIN/END query remains pending");
            assert(!strcmp(observed, "BEG") && flags_seen == 1);
        }
        host_status = (HRESULT)0x887a0005;
        assert(contexts[tier].GetData(&context, &query, NULL, 0, 0) == host_status);
        assert(!size_seen && !data_seen && !flags_seen);
        release_query(); assert(unregistrations == 1 && unrefs == 1);
    }
    puts("Production query creation and ordered context dispatch passed");
    return 0;
}
