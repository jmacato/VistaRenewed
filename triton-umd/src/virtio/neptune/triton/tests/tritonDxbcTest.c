/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Verification harness for tritonDxbc.c.
 *
 * Uses d3dcompiler_47 as the gold reference: real fxc containers validate
 * the modified-MD5 hash, and fxc's own signature chunks - converted to
 * DDI-shaped entries and fed back through tritonBuildDxbc together with the
 * fxc token stream - validate semantic naming, indexing, masks, component
 * types and chunk-variant selection element-for-element.
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Pull the unit under test into this TU so its statics (dxbcHash) are
 * reachable. */
#include "../tritonDxbc.c"

static uint32_t g_waFlags;
uint32_t npt_host_workaround_flags(void) { return g_waFlags; }

static int g_checks, g_fails;
#define CHECK(cond, ...) do {                                   \
        ++g_checks;                                             \
        if (!(cond)) {                                          \
            ++g_fails;                                          \
            printf("FAIL %s:%d: ", __FUNCTION__, __LINE__);     \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

/* ---------- d3dcompiler ---------- */

typedef HRESULT (WINAPI *PFN_D3DCompile)(
    const void*, SIZE_T, const char*, const void*, void*,
    const char*, const char*, UINT, UINT, void**, void**);

typedef struct Blob { void *p; SIZE_T cb; } Blob;

static PFN_D3DCompile s_D3DCompile;

static int compileHlsl(const char *src, const char *entry, const char *target,
                       Blob *out)
{
    /* ID3DBlob vtable: QI, AddRef, Release, GetBufferPointer, GetBufferSize */
    typedef struct { void **vtbl; } Obj;
    void *blob = NULL, *err = NULL;
    HRESULT hr = s_D3DCompile(src, strlen(src), NULL, NULL, NULL,
                              entry, target, 0, 0, &blob, &err);
    if (FAILED(hr)) {
        if (err) {
            Obj *e = (Obj *)err;
            const char *(*gbp)(void*) = (const char *(*)(void*))e->vtbl[3];
            printf("D3DCompile(%s) failed 0x%08lx: %s\n", target, hr, gbp(err));
        } else {
            printf("D3DCompile(%s) failed 0x%08lx\n", target, hr);
        }
        return 0;
    }
    Obj *b = (Obj *)blob;
    void *(*gbp)(void*) = (void *(*)(void*))b->vtbl[3];
    SIZE_T (*gbs)(void*) = (SIZE_T (*)(void*))b->vtbl[4];
    out->cb = gbs(blob);
    out->p  = malloc(out->cb);
    memcpy(out->p, gbp(blob), out->cb);
    ULONG (*rel)(void*) = (ULONG (*)(void*))b->vtbl[2];
    rel(blob);
    if (err) { Obj *e = (Obj *)err; ((ULONG (*)(void*))e->vtbl[2])(err); }
    return 1;
}

/* ---------- generic container / signature parsing ---------- */

static const void *findChunk(const void *dxbc, UINT fourcc, UINT *pcb)
{
    const UINT *h = (const UINT *)dxbc;
    UINT count = h[7];
    const UINT *offs = h + 8;
    for (UINT i = 0; i < count; ++i) {
        const UINT *c = (const UINT *)((const BYTE *)dxbc + offs[i]);
        if (c[0] == fourcc) {
            if (pcb) *pcb = c[1];
            return c + 2;
        }
    }
    return NULL;
}

typedef struct SigElem {
    UINT stream, semIdx, sysval, compType, reg, mask, used, minPrec;
    char name[64];
} SigElem;

/* Returns entry size used, 0 on absence. */
static UINT parseSig(const void *dxbc, const UINT *fourccs, UINT nFourcc,
                     SigElem *out, UINT *pn, UINT maxn)
{
    const BYTE *body = NULL;
    UINT cb = 0, fcc = 0;
    for (UINT i = 0; i < nFourcc && !body; ++i) {
        body = (const BYTE *)findChunk(dxbc, fourccs[i], &cb);
        fcc = fourccs[i];
    }
    *pn = 0;
    if (!body) return 0;

    UINT cbEntry = 24, hasStream = 0, hasPrec = 0;
    if (fcc == DXBC_BLOB_TYPE_OSG5) { cbEntry = 28; hasStream = 1; }
    if (fcc == DXBC_BLOB_TYPE_ISG1 || fcc == DXBC_BLOB_TYPE_OSG1 ||
        fcc == DXBC_BLOB_TYPE_PSG1) { cbEntry = 32; hasStream = 1; hasPrec = 1; }

    UINT n = ((const UINT *)body)[0];
    const BYTE *e = body + 8;
    for (UINT i = 0; i < n && i < maxn; ++i, e += cbEntry) {
        const BYTE *f = e + (hasStream ? 4 : 0);
        SigElem *s = &out[i];
        s->stream   = hasStream ? *(const UINT *)e : 0;
        UINT off    = *(const UINT *)(f + 0);
        s->semIdx   = *(const UINT *)(f + 4);
        s->sysval   = *(const UINT *)(f + 8);
        s->compType = *(const UINT *)(f + 12);
        s->reg      = *(const UINT *)(f + 16);
        s->mask     = *(const BYTE *)(f + 20);
        s->used     = *(const BYTE *)(f + 21);
        s->minPrec  = hasPrec ? *(const UINT *)(e + 28) : 0;
        strncpy_s(s->name, sizeof(s->name), (const char *)body + off, _TRUNCATE);
        ++*pn;
    }
    return cbEntry;
}

/* ---------- fxc signature -> DDI entries ----------
 *
 * 20-byte D3D11_1DDIARG_SIGNATURE_ENTRY2 shape: SystemValue, Register,
 * Mask(byte), Stream(byte), pad2, RegisterComponentType, MinPrecision.
 * Blob D3D_NAMEs 0..10 equal the SB_NAMEs; tess factors expand to the
 * fine-grained SB_NAMEs; PS output names (TARGET..STENCILREF, 64..69)
 * reach the DDI as UNDEFINED. */
typedef struct DdiEntry {
    UINT sysval, reg;
    BYTE mask, stream, pad0, pad1;
    UINT compType, minPrec;
} DdiEntry;

static UINT sigToDdi(const SigElem *in, UINT n, DdiEntry *out)
{
    UINT quadEdge = 11, quadIn = 15, triEdge = 17, cLine = 21;
    for (UINT i = 0; i < n; ++i) {
        UINT sv = in[i].sysval;
        if (sv >= 64) sv = 0;
        else if (sv == 11) sv = quadEdge++;         /* QUAD_EDGE -> 11..14 */
        else if (sv == 12) sv = quadIn++;           /* QUAD_INSIDE -> 15,16 */
        else if (sv == 13) sv = triEdge++;          /* TRI_EDGE -> 17..19 */
        else if (sv == 14) sv = 20;                 /* TRI_INSIDE */
        else if (sv == 15) sv = 21;                 /* LINE_DETAIL */
        else if (sv == 16) sv = 22;                 /* LINE_DENSITY */
        (void)cLine;
        out[i].sysval   = sv;
        out[i].reg      = in[i].reg;
        out[i].mask     = (BYTE)in[i].mask;
        out[i].stream   = (BYTE)in[i].stream;
        out[i].pad0 = out[i].pad1 = 0;
        out[i].compType = in[i].compType;
        out[i].minPrec  = in[i].minPrec;
    }
    return n;
}

/* Structural self-check every produced container must pass. */
static void validateContainer(const void *dxbc, SIZE_T cb)
{
    const UINT *h = (const UINT *)dxbc;
    CHECK(h[0] == DXBC_MAGIC, "magic");
    CHECK(h[5] == 1, "version");
    CHECK(h[6] == (UINT)cb, "cbTotal %u vs %zu", h[6], cb);
    UINT count = h[7];
    CHECK(count >= 3 && count <= 4, "chunk count %u", count);
    uint8_t digest[16];
    dxbcHash((const BYTE *)dxbc + 0x14, (UINT)cb - 0x14, digest);
    CHECK(memcmp(digest, h + 1, 16) == 0, "hash self-consistent");
    for (UINT i = 0; i < count; ++i) {
        UINT off = h[8 + i];
        CHECK(off + 8 <= cb, "chunk %u offset", i);
        const UINT *c = (const UINT *)((const BYTE *)dxbc + off);
        CHECK(off + 8 + c[1] <= cb, "chunk %u size", i);
        CHECK((c[1] & 3) == 0, "chunk %u alignment", i);
    }
}

/* fxc hash cross-check: our dxbcHash over a genuine container must equal
 * its embedded digest. */
static void checkFxcHash(const Blob *b)
{
    uint8_t digest[16];
    dxbcHash((const BYTE *)b->p + 0x14, (UINT)b->cb - 0x14, digest);
    CHECK(memcmp(digest, (const BYTE *)b->p + 4, 16) == 0,
          "hash matches fxc (size %zu, size%%64=%zu)", b->cb, b->cb % 64);
}

/* Down-convert to the 12-byte pre-11.1 entry: no stream, component type
 * or min precision. */
static void packDdi12(const DdiEntry *in, UINT n, BYTE *out)
{
    for (UINT i = 0; i < n; ++i) {
        BYTE *e = out + (size_t)i * 12;
        *(UINT *)(e + 0) = in[i].sysval;
        *(UINT *)(e + 4) = in[i].reg;
        e[8] = in[i].mask; e[9] = e[10] = e[11] = 0;
    }
}

/* Rebuild a container from an fxc blob's own token stream + DDI-ized
 * signatures. */
static void *rebuild(const Blob *fxc, SIZE_T *pcb,
                     int fZeroIn, int fZeroOut, UINT stride)
{
    static const UINT codeTags[] = { DXBC_BLOB_TYPE_SHEX, DXBC_BLOB_TYPE_SHDR };
    static const UINT inTags[]  = { DXBC_BLOB_TYPE_ISG1, DXBC_BLOB_TYPE_ISGN };
    static const UINT outTags[] = { DXBC_BLOB_TYPE_OSG1, DXBC_BLOB_TYPE_OSG5,
                                    DXBC_BLOB_TYPE_OSGN };
    static const UINT pcTags[]  = { DXBC_BLOB_TYPE_PSG1, DXBC_BLOB_TYPE_PCSG };

    UINT cbCode = 0;
    const UINT *tokens = (const UINT *)findChunk(fxc->p, codeTags[0], &cbCode);
    if (!tokens) tokens = (const UINT *)findChunk(fxc->p, codeTags[1], &cbCode);
    CHECK(tokens != NULL, "code chunk present");
    if (!tokens) return NULL;

    SigElem si[128], so[128], sp[128];
    UINT ni = 0, no = 0, np = 0;
    parseSig(fxc->p, inTags, 2, si, &ni, 128);
    parseSig(fxc->p, outTags, 3, so, &no, 128);
    parseSig(fxc->p, pcTags, 2, sp, &np, 128);

    DdiEntry di[128], dout[128], dp[128];
    sigToDdi(si, ni, di);
    sigToDdi(so, no, dout);
    sigToDdi(sp, np, dp);

    if (fZeroIn) ni = 0;
    if (fZeroOut) no = 0;

    BYTE di12[128 * 12], dout12[128 * 12], dp12[128 * 12];
    const void *pi = di, *po = dout, *pp = dp;
    if (stride == 12) {
        packDdi12(di, ni, di12);
        packDdi12(dout, no, dout12);
        packDdi12(dp, np, dp12);
        pi = di12; po = dout12; pp = dp12;
    }

    void *built = tritonBuildDxbc(tokens, cbCode,
                                  ni ? pi : NULL, ni,
                                  no ? po : NULL, no,
                                  np ? pp : NULL, np,
                                  stride, pcb);
    CHECK(built != NULL, "tritonBuildDxbc");
    if (built) validateContainer(built, *pcb);
    return built;
}

static const SigElem *findElem(const SigElem *a, UINT n, UINT stream, UINT reg,
                               UINT maskBit)
{
    for (UINT i = 0; i < n; ++i)
        if (a[i].stream == stream && a[i].reg == reg
            && (a[i].mask & maskBit))
            return &a[i];
    return NULL;
}

/* Compare our signature against fxc's element-for-element on the fields with
 * a canonical value (arbitrary-semantic NAMES legitimately differ: fxc keeps
 * the HLSL name, triton says ATTRIBn). */
static void compareSig(const char *what, const SigElem *fx, UINT nfx,
                       const SigElem *tr, UINT ntr)
{
    CHECK(nfx == ntr, "%s: element count fxc=%u triton=%u", what, nfx, ntr);
    for (UINT i = 0; i < nfx; ++i) {
        const SigElem *f = &fx[i];
        const SigElem *t = findElem(tr, ntr, f->stream, f->reg,
                                    f->mask ? f->mask : 1);
        if (f->reg == 0xFFFFFFFFu) {
            /* register-less: match by name */
            t = NULL;
            for (UINT j = 0; j < ntr; ++j)
                if (strcmp(tr[j].name, f->name) == 0) { t = &tr[j]; break; }
        }
        CHECK(t != NULL, "%s: fxc elem %u (%s%u reg %d) present",
              what, i, f->name, f->semIdx, (int)f->reg);
        if (!t) continue;
        CHECK(t->mask == f->mask, "%s %s%u: mask %x vs fxc %x",
              what, f->name, f->semIdx, t->mask, f->mask);
        CHECK(t->sysval == f->sysval, "%s %s%u: sysval %u vs fxc %u",
              what, f->name, f->semIdx, t->sysval, f->sysval);
        CHECK(t->compType == f->compType, "%s %s%u: compType %u vs fxc %u",
              what, f->name, f->semIdx, t->compType, f->compType);
        if (_strnicmp(f->name, "SV_", 3) == 0) {
            CHECK(strcmp(t->name, f->name) == 0, "%s: name %s vs fxc %s",
                  what, t->name, f->name);
            CHECK(t->semIdx == f->semIdx, "%s %s: semIdx %u vs fxc %u",
                  what, f->name, t->semIdx, f->semIdx);
        }
    }
}

static void compareVsFxc(const char *what, const Blob *fxc, int fZeroIn,
                         int fZeroOut, UINT stride,
                         UINT expectOutFcc, UINT expectCodeFcc)
{
    SIZE_T cb = 0;
    void *built = rebuild(fxc, &cb, fZeroIn, fZeroOut, stride);
    if (!built) return;

    CHECK(findChunk(built, expectCodeFcc, NULL) != NULL,
          "%s: code chunk fourcc", what);
    if (expectOutFcc)
        CHECK(findChunk(built, expectOutFcc, NULL) != NULL,
              "%s: output signature fourcc", what);

    static const UINT inTags[]  = { DXBC_BLOB_TYPE_ISG1, DXBC_BLOB_TYPE_ISGN };
    static const UINT outTags[] = { DXBC_BLOB_TYPE_OSG1, DXBC_BLOB_TYPE_OSG5,
                                    DXBC_BLOB_TYPE_OSGN };
    static const UINT pcTags[]  = { DXBC_BLOB_TYPE_PSG1, DXBC_BLOB_TYPE_PCSG };

    SigElem fi[128], fo[128], fp[128], ti[128], to[128], tp[128];
    UINT nfi, nfo, nfp, nti, nto, ntp;
    parseSig(fxc->p, inTags, 2, fi, &nfi, 128);
    parseSig(fxc->p, outTags, 3, fo, &nfo, 128);
    parseSig(fxc->p, pcTags, 2, fp, &nfp, 128);
    parseSig(built, inTags, 2, ti, &nti, 128);
    parseSig(built, outTags, 3, to, &nto, 128);
    parseSig(built, pcTags, 2, tp, &ntp, 128);

    char buf[128];
    sprintf_s(buf, sizeof(buf), "%s ISGN", what);
    compareSig(buf, fi, nfi, ti, nti);
    sprintf_s(buf, sizeof(buf), "%s OSGN", what);
    compareSig(buf, fo, nfo, to, nto);
    sprintf_s(buf, sizeof(buf), "%s PCSG", what);
    compareSig(buf, fp, nfp, tp, ntp);

    HeapFree(GetProcessHeap(), 0, built);
}

/* ---------- tests ---------- */

static const char HLSL_PS_BASIC[] =
    "float4 main(float4 c : COLOR) : SV_Target { return c; }";

static const char HLSL_PS_SPARSE_MRT[] =
    "struct O { float4 a : SV_Target0; float4 b : SV_Target3; };"
    "O main(float4 p : SV_Position, float4 c : COLOR) {"
    "  O o; o.a = c; o.b = c * 2; return o; }";

static const char HLSL_PS_REORDERED_MRT[] =
    "struct O { float4 b : SV_Target1; float4 a : SV_Target0; };"
    "O main(float4 c : COLOR) { O o; o.a = c; o.b = c * 3; return o; }";

static const char HLSL_PS_DEPTHGE_ICB[] =
    "static const float4 k[4] = { float4(1,0,0,0), float4(0,1,0,0),"
    "                             float4(0,0,1,0), float4(0,0,0,1) };"
    "float4 main(centroid noperspective float4 p : SV_Position,"
    "            out float d : SV_DepthGreaterEqual) : SV_Target {"
    "  d = k[(int)p.x & 3].x; return k[(int)p.y & 3]; }";

static const char HLSL_PS_COVERAGE[] =
    "float4 main(float4 p : SV_Position, out uint cov : SV_Coverage)"
    "    : SV_Target { cov = (uint)p.x; return p; }";

static const char HLSL_PS_INT_TARGET[] =
    "uint4 main(float4 p : SV_Position) : SV_Target1 { return (uint4)p; }";

static const char HLSL_VS_BASIC[] =
    "struct O { float4 p : SV_Position; float2 t : TEXCOORD0;"
    "           float3 n : NORMAL; };"
    "O main(float3 pos : POSITION, float2 t : TEXCOORD0, float3 n : NORMAL,"
    "       uint vid : SV_VertexID) {"
    "  O o; o.p = float4(pos, vid); o.t = t; o.n = n; return o; }";

static const char HLSL_VS_CLIP[] =
    "struct O { float4 p : SV_Position; float2 c : SV_ClipDistance0;"
    "           float2 d : SV_ClipDistance1; float4 t : TEXCOORD0; };"
    "O main(float4 pos : POSITION) {"
    "  O o; o.p = pos; o.c = pos.xy; o.d = pos.zw; o.t = pos; return o; }";

static const char HLSL_GS_BASIC[] =
    "struct V { float4 p : SV_Position; float3 t : TEXCOORD0; };"
    "[maxvertexcount(3)]"
    "void main(triangle V i[3], inout TriangleStream<V> s) {"
    "  for (int k = 0; k < 3; ++k) s.Append(i[k]); }";

static const char HLSL_GS_MULTISTREAM[] =
    "struct V { float4 p : SV_Position; float4 t : TEXCOORD0; };"
    "[maxvertexcount(4)]"
    "void main(point V i[1], inout PointStream<V> s0,"
    "          inout PointStream<V> s1) {"
    "  s0.Append(i[0]); s1.Append(i[0]); }";

static const char HLSL_GS_PRIMID[] =
    "struct V { float4 p : SV_Position; };"
    "[maxvertexcount(3)]"
    "void main(triangle V i[3], uint pid : SV_PrimitiveID,"
    "          inout TriangleStream<V> s) {"
    "  V o; o.p = i[0].p + pid; s.Append(o); }";

static const char HLSL_GS_INSTANCED[] =
    "struct V { float4 p : SV_Position; };"
    "[maxvertexcount(3)] [instance(2)]"
    "void main(triangle V i[3], uint gid : SV_GSInstanceID,"
    "          inout TriangleStream<V> s) {"
    "  V o; o.p = i[0].p + gid; s.Append(o); }";

static const char HLSL_HS_QUAD[] =
    "struct CP { float3 p : POSITION; };"
    "struct K { float e[4] : SV_TessFactor;"
    "           float i[2] : SV_InsideTessFactor;"
    "           float3 c : CENTER; };"
    "K khc(InputPatch<CP, 4> p) {"
    "  K k; k.e[0] = k.e[1] = k.e[2] = k.e[3] = 4;"
    "  k.i[0] = k.i[1] = 2; k.c = p[0].p; return k; }"
    "[domain(\"quad\")] [partitioning(\"integer\")]"
    "[outputtopology(\"triangle_cw\")] [outputcontrolpoints(4)]"
    "[patchconstantfunc(\"khc\")]"
    "CP main(InputPatch<CP, 4> p, uint i : SV_OutputControlPointID) {"
    "  return p[i]; }";

static const char HLSL_DS_QUAD[] =
    "struct CP { float3 p : POSITION; };"
    "struct K { float e[4] : SV_TessFactor;"
    "           float i[2] : SV_InsideTessFactor;"
    "           float3 c : CENTER; };"
    "struct O { float4 p : SV_Position; };"
    "[domain(\"quad\")]"
    "O main(K k, float2 uv : SV_DomainLocation,"
    "       const OutputPatch<CP, 4> p) {"
    "  O o; o.p = float4(lerp(lerp(p[0].p, p[1].p, uv.x),"
    "                         lerp(p[3].p, p[2].p, uv.x), uv.y) + k.c, 1);"
    "  return o; }";

static void testHashes(void)
{
    static const struct { const char *src, *entry, *target; } cases[] = {
        { HLSL_PS_BASIC,      "main", "ps_4_0" },
        { HLSL_PS_BASIC,      "main", "ps_5_0" },
        { HLSL_PS_SPARSE_MRT, "main", "ps_5_0" },
        { HLSL_VS_BASIC,      "main", "vs_4_0" },
        { HLSL_VS_BASIC,      "main", "vs_5_0" },
        { HLSL_GS_BASIC,      "main", "gs_5_0" },
        { HLSL_HS_QUAD,       "main", "hs_5_0" },
        { HLSL_DS_QUAD,       "main", "ds_5_0" },
    };
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); ++i) {
        Blob b;
        if (!compileHlsl(cases[i].src, cases[i].entry, cases[i].target, &b)) {
            CHECK(0, "compile %s", cases[i].target);
            continue;
        }
        checkFxcHash(&b);
        free(b.p);
    }
}

static void testRebuilds(void)
{
    static const struct {
        const char *what, *src, *target;
        int zeroIn, zeroOut;
        UINT stride;
        UINT outFcc, codeFcc;
    } cases[] = {
        { "ps40",       HLSL_PS_BASIC,         "ps_4_0", 0, 0, 20,
          DXBC_BLOB_TYPE_OSGN, DXBC_BLOB_TYPE_SHDR },
        { "ps50",       HLSL_PS_BASIC,         "ps_5_0", 0, 0, 20,
          DXBC_BLOB_TYPE_OSGN, DXBC_BLOB_TYPE_SHEX },
        { "sparseMRT",  HLSL_PS_SPARSE_MRT,    "ps_5_0", 0, 0, 20,
          DXBC_BLOB_TYPE_OSGN, DXBC_BLOB_TYPE_SHEX },
        { "reordMRT",   HLSL_PS_REORDERED_MRT, "ps_5_0", 0, 0, 20,
          DXBC_BLOB_TYPE_OSGN, DXBC_BLOB_TYPE_SHEX },
        { "depthGEicb", HLSL_PS_DEPTHGE_ICB,   "ps_5_0", 0, 0, 20,
          DXBC_BLOB_TYPE_OSGN, DXBC_BLOB_TYPE_SHEX },
        { "coverage",   HLSL_PS_COVERAGE,      "ps_5_0", 0, 0, 20,
          DXBC_BLOB_TYPE_OSGN, DXBC_BLOB_TYPE_SHEX },
        { "intTarget",  HLSL_PS_INT_TARGET,    "ps_5_0", 0, 0, 20,
          DXBC_BLOB_TYPE_OSGN, DXBC_BLOB_TYPE_SHEX },
        { "vsClip",     HLSL_VS_CLIP,          "vs_5_0", 0, 0, 20,
          DXBC_BLOB_TYPE_OSGN, DXBC_BLOB_TYPE_SHEX },
        { "gs40",       HLSL_GS_BASIC,         "gs_4_0", 0, 0, 20,
          DXBC_BLOB_TYPE_OSGN, DXBC_BLOB_TYPE_SHDR },
        { "gs50",       HLSL_GS_BASIC,         "gs_5_0", 0, 0, 20,
          DXBC_BLOB_TYPE_OSG5, DXBC_BLOB_TYPE_SHEX },
        { "gsMulti",    HLSL_GS_MULTISTREAM,   "gs_5_0", 0, 0, 20,
          DXBC_BLOB_TYPE_OSG5, DXBC_BLOB_TYPE_SHEX },
        /* Pre-11.1 12-byte entries carry no stream byte; the declarations
         * must attribute every element to its stream. */
        { "gsMulti12",  HLSL_GS_MULTISTREAM,   "gs_5_0", 0, 0, 12,
          DXBC_BLOB_TYPE_OSG5, DXBC_BLOB_TYPE_SHEX },
        { "gsPrim",     HLSL_GS_PRIMID,        "gs_5_0", 0, 0, 20,
          DXBC_BLOB_TYPE_OSG5, DXBC_BLOB_TYPE_SHEX },
        { "gsInst",     HLSL_GS_INSTANCED,     "gs_5_0", 0, 0, 20,
          DXBC_BLOB_TYPE_OSG5, DXBC_BLOB_TYPE_SHEX },
        /* Signature-less create (stream-output GS shape): everything must be
         * recovered from the declarations. */
        { "gs40zero",   HLSL_GS_BASIC,         "gs_4_0", 1, 1, 20,
          DXBC_BLOB_TYPE_OSGN, DXBC_BLOB_TYPE_SHDR },
        { "gs50zero",   HLSL_GS_BASIC,         "gs_5_0", 1, 1, 20,
          DXBC_BLOB_TYPE_OSG5, DXBC_BLOB_TYPE_SHEX },
        { "gsMultiZero", HLSL_GS_MULTISTREAM,  "gs_5_0", 1, 1, 20,
          DXBC_BLOB_TYPE_OSG5, DXBC_BLOB_TYPE_SHEX },
        { "gsPrimZero", HLSL_GS_PRIMID,        "gs_5_0", 1, 1, 20,
          DXBC_BLOB_TYPE_OSG5, DXBC_BLOB_TYPE_SHEX },
        { "gsInstZero", HLSL_GS_INSTANCED,     "gs_5_0", 1, 1, 20,
          DXBC_BLOB_TYPE_OSG5, DXBC_BLOB_TYPE_SHEX },
        { "hsQuad",     HLSL_HS_QUAD,          "hs_5_0", 0, 0, 20,
          DXBC_BLOB_TYPE_OSGN, DXBC_BLOB_TYPE_SHEX },
        { "dsQuad",     HLSL_DS_QUAD,          "ds_5_0", 0, 0, 20,
          DXBC_BLOB_TYPE_OSGN, DXBC_BLOB_TYPE_SHEX },
    };
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); ++i) {
        Blob b;
        if (!compileHlsl(cases[i].src, "main", cases[i].target, &b)) {
            CHECK(0, "compile %s", cases[i].what);
            continue;
        }
        compareVsFxc(cases[i].what, &b, cases[i].zeroIn, cases[i].zeroOut,
                     cases[i].stride, cases[i].outFcc, cases[i].codeFcc);
        free(b.p);
    }
}

/* min-precision DDI entries -> ISG1/OSG1 emission, and the reconciler
 * against both ISGN and ISG1 forms. */
static void testMinPrecAndReconcile(void)
{
    Blob b;
    if (!compileHlsl(HLSL_VS_BASIC, "main", "vs_5_0", &b)) {
        CHECK(0, "compile vs for reconcile");
        return;
    }

    static const UINT inTags[]  = { DXBC_BLOB_TYPE_ISG1, DXBC_BLOB_TYPE_ISGN };

    for (int fPrec = 0; fPrec <= 1; ++fPrec) {
        UINT cbCode = 0;
        const UINT *tokens =
            (const UINT *)findChunk(b.p, DXBC_BLOB_TYPE_SHEX, &cbCode);
        SigElem si[128], so[128];
        UINT ni, no;
        parseSig(b.p, inTags, 2, si, &ni, 128);
        static const UINT outTags[] = { DXBC_BLOB_TYPE_OSGN };
        parseSig(b.p, outTags, 1, so, &no, 128);
        DdiEntry di[128], dout[128];
        sigToDdi(si, ni, di);
        sigToDdi(so, no, dout);
        if (fPrec)
            for (UINT i = 0; i < ni; ++i)
                if (di[i].sysval == 0)
                    di[i].minPrec = 1;  /* FLOAT_16 */

        SIZE_T cb = 0;
        void *built = tritonBuildDxbc(tokens, cbCode, di, ni, dout, no,
                                      NULL, 0, 20, &cb);
        CHECK(built != NULL, "build (prec=%d)", fPrec);
        if (!built) continue;
        validateContainer(built, cb);
        CHECK(findChunk(built, fPrec ? DXBC_BLOB_TYPE_ISG1 : DXBC_BLOB_TYPE_ISGN,
                        NULL) != NULL, "input chunk variant (prec=%d)", fPrec);

        /* Reconcile every ATTRIB register to sint and verify. */
        unsigned char compReg[32];
        memset(compReg, 0, sizeof(compReg));
        SigElem ti[128];
        UINT nti;
        parseSig(built, inTags, 2, ti, &nti, 128);
        UINT nGeneric = 0;
        for (UINT i = 0; i < nti; ++i)
            if (ti[i].sysval == 0 && ti[i].reg < 32) {
                compReg[ti[i].reg] = 2;
                ++nGeneric;
            }
        CHECK(nGeneric > 0, "vs has generic inputs (prec=%d)", fPrec);

        SIZE_T cbRec = 0;
        void *rec = tritonReconcileVsInputSig(built, cb, compReg, &cbRec);
        CHECK(rec != NULL, "reconcile produced a copy (prec=%d)", fPrec);
        if (rec) {
            CHECK(cbRec == cb, "reconcile size (prec=%d)", fPrec);
            validateContainer(rec, cbRec);
            SigElem ri[128];
            UINT nri;
            parseSig(rec, inTags, 2, ri, &nri, 128);
            for (UINT i = 0; i < nri; ++i)
                if (ri[i].sysval == 0 && ri[i].reg < 32)
                    CHECK(ri[i].compType == 2,
                          "reconciled compType reg %u (prec=%d)",
                          ri[i].reg, fPrec);
            HeapFree(GetProcessHeap(), 0, rec);
        }
        HeapFree(GetProcessHeap(), 0, built);
    }
    free(b.p);
}

/* The scalar-input widen workaround (flag-gated). */
static void testWidenFlag(void)
{
    static const char *src =
        "float4 main(float s : PSIZE, uint vid : SV_VertexID)"
        "    : SV_Position { return float4(s, (float)vid, 0, 1); }";
    Blob b;
    if (!compileHlsl(src, "main", "vs_5_0", &b)) {
        CHECK(0, "compile widen vs");
        return;
    }
    UINT cbCode = 0;
    const UINT *tokens =
        (const UINT *)findChunk(b.p, DXBC_BLOB_TYPE_SHEX, &cbCode);
    static const UINT inTags[]  = { DXBC_BLOB_TYPE_ISGN };
    static const UINT outTags[] = { DXBC_BLOB_TYPE_OSGN };
    SigElem si[16], so[16];
    UINT ni, no;
    parseSig(b.p, inTags, 1, si, &ni, 16);
    parseSig(b.p, outTags, 1, so, &no, 16);
    DdiEntry di[16], dout[16];
    sigToDdi(si, ni, di);
    sigToDdi(so, no, dout);

    for (int fWiden = 0; fWiden <= 1; ++fWiden) {
        g_waFlags = fWiden ? NPT_WA_WIDEN_SCALAR_VS_INPUT_MASK : 0;
        SIZE_T cb = 0;
        void *built = tritonBuildDxbc(tokens, cbCode, di, ni, dout, no,
                                      NULL, 0, 20, &cb);
        CHECK(built != NULL, "build widen=%d", fWiden);
        if (!built) continue;
        validateContainer(built, cb);
        SigElem ti[16];
        UINT nti;
        parseSig(built, inTags, 1, ti, &nti, 16);
        UINT nScalar = 0, nVid = 0;
        for (UINT i = 0; i < nti; ++i) {
            if (ti[i].sysval == 0) {
                ++nScalar;
                CHECK(ti[i].mask == (fWiden ? 0x3u : 0x1u),
                      "scalar input mask %x widen=%d", ti[i].mask, fWiden);
            }
            if (ti[i].sysval == 6) {  /* SV_VertexID must never widen */
                ++nVid;
                CHECK(ti[i].mask == 0x1u, "vertexid mask %x", ti[i].mask);
            }
        }
        CHECK(nScalar == 1 && nVid == 1,
              "widen shader shape (%u scalar, %u vid)", nScalar, nVid);
        HeapFree(GetProcessHeap(), 0, built);
    }
    g_waFlags = 0;
    free(b.p);
}

int main(void)
{
    HMODULE hCompiler = LoadLibraryA("d3dcompiler_47.dll");
    if (!hCompiler) {
        printf("d3dcompiler_47.dll not found\n");
        return 2;
    }
    s_D3DCompile = (PFN_D3DCompile)GetProcAddress(hCompiler, "D3DCompile");
    if (!s_D3DCompile) {
        printf("D3DCompile not found\n");
        return 2;
    }

    testHashes();
    testRebuilds();
    testMinPrecAndReconcile();
    testWidenFlag();

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
