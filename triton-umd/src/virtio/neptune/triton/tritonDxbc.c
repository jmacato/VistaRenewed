/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * DXBC container builder.
 *
 * The D3D11 DDI hands shader create handlers a raw tokenized program:
 *   pTokens[0]      = VGPU10ProgramToken (version + program type)
 *   pTokens[1]      = total length in DWORDs
 *   pTokens[2..N-1] = instruction tokens
 * plus separately-passed signature arrays.
 *
 * ID3D11Device1::CreateXxxShader wants a DXBC container (the binary
 * blob fxc/dxc produces): magic + modified-MD5 hash + version + size +
 * chunk offsets, followed by input / output / (HS/DS) patch-constant
 * signature chunks and the code chunk. Chunk variants match fxc: SHEX
 * for SM5 code (SHDR for SM4), OSG5 for an SM5 GS output signature,
 * ISG1/OSG1/PSG1 when the shader uses minimum precision. This file
 * builds that container around the DDI tokens.
 *
 * Little-endian only (mingw-w64 → Windows x64).
 */

#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "triton_log.h"
#include "npt_workaround.h"  /* npt_host_workaround_flags() + NPT_WA_* bits */
#include "tritonDxbc.h"
#include "tritonDxbcSignature.h"

/* ---------- DXBC container format ----------
 * Field offsets verified by _Static_assert at the bottom of this file. */

#define DXBC_MAGIC          0x43425844u  /* 'DXBC' */
#define DXBC_BLOB_TYPE_ISGN 0x4E475349u  /* 'ISGN' */
#define DXBC_BLOB_TYPE_ISG1 0x31475349u  /* 'ISG1' — ISGN + Stream + MinPrecision */
#define DXBC_BLOB_TYPE_OSGN 0x4E47534Fu  /* 'OSGN' */
#define DXBC_BLOB_TYPE_OSG5 0x3547534Fu  /* 'OSG5' — OSGN + Stream (SM5 GS) */
#define DXBC_BLOB_TYPE_OSG1 0x3147534Fu  /* 'OSG1' — OSGN + Stream + MinPrecision */
#define DXBC_BLOB_TYPE_PCSG 0x47534350u  /* 'PCSG' */
#define DXBC_BLOB_TYPE_PSG1 0x31475350u  /* 'PSG1' — PCSG + Stream + MinPrecision */
#define DXBC_BLOB_TYPE_SHDR 0x52444853u  /* 'SHDR' — SM4 code */
#define DXBC_BLOB_TYPE_SHEX 0x58454853u  /* 'SHEX' — SM5 code */

typedef struct DXBCHeader {
    uint32_t u32DXBC;            /* DXBC_MAGIC */
    uint8_t  au8Hash[16];        /* Modified-MD5 of cbTotal-0x14 bytes from u32Version onward. */
    uint32_t u32Version;         /* 1 */
    uint32_t cbTotal;            /* Total file size in bytes, including this header. */
    uint32_t cBlob;              /* Number of blob offsets following. */
    uint32_t aBlobOffset[1];     /* [cBlob] offsets from file start to each DXBCBlobHeader. */
} DXBCHeader;

typedef struct DXBCBlobHeader {
    uint32_t u32BlobType;        /* FourCC. */
    uint32_t cbBlob;             /* Bytes of payload following this header. */
} DXBCBlobHeader;

typedef struct DXBCBlobIOSGNElement {
    uint32_t offElementName;     /* Offset of ASCIIZ name from start of IOSGN blob. */
    uint32_t idxSemantic;
    uint32_t enmSystemValue;     /* D3D_NAME / DxilProgramSigSemantic. */
    uint32_t enmComponentType;   /* 1=uint32, 2=int32, 3=float32. */
    uint32_t idxRegister;
    uint32_t maskAndUsed;        /* Bits [7:0]=Mask, [15:8]=Used/NeverWrites mask. */
} DXBCBlobIOSGNElement;

typedef struct DXBCBlobIOSGN {
    uint32_t cElement;
    uint32_t offElement;         /* 8 — element array starts immediately after this. */
    /* Followed by DXBCBlobIOSGNElement[cElement], then ASCIIZ names. */
} DXBCBlobIOSGN;

/* ---------- Modified MD5 ----------
 *
 * Standard MD5 inner loop, but `dxbcHash` uses a non-standard terminator
 * block that D3DCompiler expects. Standard MD5 padding will not produce
 * the same hash and Microsoft's runtime / dxvk validator will reject
 * the container. */

#define F1(x, y, z) (z ^ (x & (y ^ z)))
#define F2(x, y, z) F1(z, x, y)
#define F3(x, y, z) (x ^ y ^ z)
#define F4(x, y, z) (y ^ (x | ~z))
#define MD5STEP(f, w, x, y, z, data, s) \
    ( w += f(x, y, z) + (data),  w = w<<s | w>>(32-s),  w += x )

typedef struct DxbcMd5Ctx {
    uint32_t buf[4];
    uint32_t bits[2];
    uint8_t  in[64];
} DxbcMd5Ctx;

static void md5Transform(uint32_t buf[4], const uint32_t in[16])
{
    uint32_t a = buf[0], b = buf[1], c = buf[2], d = buf[3];

    MD5STEP(F1, a, b, c, d, in[ 0] + 0xd76aa478,  7);
    MD5STEP(F1, d, a, b, c, in[ 1] + 0xe8c7b756, 12);
    MD5STEP(F1, c, d, a, b, in[ 2] + 0x242070db, 17);
    MD5STEP(F1, b, c, d, a, in[ 3] + 0xc1bdceee, 22);
    MD5STEP(F1, a, b, c, d, in[ 4] + 0xf57c0faf,  7);
    MD5STEP(F1, d, a, b, c, in[ 5] + 0x4787c62a, 12);
    MD5STEP(F1, c, d, a, b, in[ 6] + 0xa8304613, 17);
    MD5STEP(F1, b, c, d, a, in[ 7] + 0xfd469501, 22);
    MD5STEP(F1, a, b, c, d, in[ 8] + 0x698098d8,  7);
    MD5STEP(F1, d, a, b, c, in[ 9] + 0x8b44f7af, 12);
    MD5STEP(F1, c, d, a, b, in[10] + 0xffff5bb1, 17);
    MD5STEP(F1, b, c, d, a, in[11] + 0x895cd7be, 22);
    MD5STEP(F1, a, b, c, d, in[12] + 0x6b901122,  7);
    MD5STEP(F1, d, a, b, c, in[13] + 0xfd987193, 12);
    MD5STEP(F1, c, d, a, b, in[14] + 0xa679438e, 17);
    MD5STEP(F1, b, c, d, a, in[15] + 0x49b40821, 22);

    MD5STEP(F2, a, b, c, d, in[ 1] + 0xf61e2562,  5);
    MD5STEP(F2, d, a, b, c, in[ 6] + 0xc040b340,  9);
    MD5STEP(F2, c, d, a, b, in[11] + 0x265e5a51, 14);
    MD5STEP(F2, b, c, d, a, in[ 0] + 0xe9b6c7aa, 20);
    MD5STEP(F2, a, b, c, d, in[ 5] + 0xd62f105d,  5);
    MD5STEP(F2, d, a, b, c, in[10] + 0x02441453,  9);
    MD5STEP(F2, c, d, a, b, in[15] + 0xd8a1e681, 14);
    MD5STEP(F2, b, c, d, a, in[ 4] + 0xe7d3fbc8, 20);
    MD5STEP(F2, a, b, c, d, in[ 9] + 0x21e1cde6,  5);
    MD5STEP(F2, d, a, b, c, in[14] + 0xc33707d6,  9);
    MD5STEP(F2, c, d, a, b, in[ 3] + 0xf4d50d87, 14);
    MD5STEP(F2, b, c, d, a, in[ 8] + 0x455a14ed, 20);
    MD5STEP(F2, a, b, c, d, in[13] + 0xa9e3e905,  5);
    MD5STEP(F2, d, a, b, c, in[ 2] + 0xfcefa3f8,  9);
    MD5STEP(F2, c, d, a, b, in[ 7] + 0x676f02d9, 14);
    MD5STEP(F2, b, c, d, a, in[12] + 0x8d2a4c8a, 20);

    MD5STEP(F3, a, b, c, d, in[ 5] + 0xfffa3942,  4);
    MD5STEP(F3, d, a, b, c, in[ 8] + 0x8771f681, 11);
    MD5STEP(F3, c, d, a, b, in[11] + 0x6d9d6122, 16);
    MD5STEP(F3, b, c, d, a, in[14] + 0xfde5380c, 23);
    MD5STEP(F3, a, b, c, d, in[ 1] + 0xa4beea44,  4);
    MD5STEP(F3, d, a, b, c, in[ 4] + 0x4bdecfa9, 11);
    MD5STEP(F3, c, d, a, b, in[ 7] + 0xf6bb4b60, 16);
    MD5STEP(F3, b, c, d, a, in[10] + 0xbebfbc70, 23);
    MD5STEP(F3, a, b, c, d, in[13] + 0x289b7ec6,  4);
    MD5STEP(F3, d, a, b, c, in[ 0] + 0xeaa127fa, 11);
    MD5STEP(F3, c, d, a, b, in[ 3] + 0xd4ef3085, 16);
    MD5STEP(F3, b, c, d, a, in[ 6] + 0x04881d05, 23);
    MD5STEP(F3, a, b, c, d, in[ 9] + 0xd9d4d039,  4);
    MD5STEP(F3, d, a, b, c, in[12] + 0xe6db99e5, 11);
    MD5STEP(F3, c, d, a, b, in[15] + 0x1fa27cf8, 16);
    MD5STEP(F3, b, c, d, a, in[ 2] + 0xc4ac5665, 23);

    MD5STEP(F4, a, b, c, d, in[ 0] + 0xf4292244,  6);
    MD5STEP(F4, d, a, b, c, in[ 7] + 0x432aff97, 10);
    MD5STEP(F4, c, d, a, b, in[14] + 0xab9423a7, 15);
    MD5STEP(F4, b, c, d, a, in[ 5] + 0xfc93a039, 21);
    MD5STEP(F4, a, b, c, d, in[12] + 0x655b59c3,  6);
    MD5STEP(F4, d, a, b, c, in[ 3] + 0x8f0ccc92, 10);
    MD5STEP(F4, c, d, a, b, in[10] + 0xffeff47d, 15);
    MD5STEP(F4, b, c, d, a, in[ 1] + 0x85845dd1, 21);
    MD5STEP(F4, a, b, c, d, in[ 8] + 0x6fa87e4f,  6);
    MD5STEP(F4, d, a, b, c, in[15] + 0xfe2ce6e0, 10);
    MD5STEP(F4, c, d, a, b, in[ 6] + 0xa3014314, 15);
    MD5STEP(F4, b, c, d, a, in[13] + 0x4e0811a1, 21);
    MD5STEP(F4, a, b, c, d, in[ 4] + 0xf7537e82,  6);
    MD5STEP(F4, d, a, b, c, in[11] + 0xbd3af235, 10);
    MD5STEP(F4, c, d, a, b, in[ 2] + 0x2ad7d2bb, 15);
    MD5STEP(F4, b, c, d, a, in[ 9] + 0xeb86d391, 21);

    buf[0] += a; buf[1] += b; buf[2] += c; buf[3] += d;
}

static void md5Init(DxbcMd5Ctx *pCtx)
{
    pCtx->buf[0]  = 0x67452301;
    pCtx->buf[1]  = 0xefcdab89;
    pCtx->buf[2]  = 0x98badcfe;
    pCtx->buf[3]  = 0x10325476;
    pCtx->bits[0] = 0;
    pCtx->bits[1] = 0;
}

static void md5Update(DxbcMd5Ctx *pCtx, const void *pvBuf, size_t len)
{
    const uint8_t *buf = (const uint8_t *)pvBuf;
    uint32_t t = pCtx->bits[0];
    if ((pCtx->bits[0] = t + ((uint32_t)len << 3)) < t)
        pCtx->bits[1]++;
    pCtx->bits[1] += (uint32_t)(len >> 29);

    t = (t >> 3) & 0x3f;
    if (t) {
        uint8_t *p = pCtx->in + t;
        t = 64 - t;
        if (len < t) {
            memcpy(p, buf, len);
            return;
        }
        memcpy(p, buf, t);
        md5Transform(pCtx->buf, (const uint32_t *)pCtx->in);
        buf += t;
        len -= t;
    }

    if (!((uintptr_t)buf & 0x3)) {
        while (len >= 64) {
            md5Transform(pCtx->buf, (const uint32_t *)buf);
            buf += 64;
            len -= 64;
        }
    } else {
        while (len >= 64) {
            memcpy(pCtx->in, buf, 64);
            md5Transform(pCtx->buf, (const uint32_t *)pCtx->in);
            buf += 64;
            len -= 64;
        }
    }

    memcpy(pCtx->in, buf, len);
}

static void dxbcHash(const void *pvData, uint32_t cbData, uint8_t pabDigest[16])
{
    enum { kBlockSize = 64 };
    uint8_t au8BlockBuffer[kBlockSize];
    static const uint8_t s_au8Padding[kBlockSize] = {
        0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

    DxbcMd5Ctx ctx;
    md5Init(&ctx);

    const uint8_t *pu8 = (const uint8_t *)pvData;
    size_t cbRemaining = cbData;
    size_t cbComplete  = cbData & ~(size_t)(kBlockSize - 1);
    md5Update(&ctx, pu8, cbComplete);
    pu8 += cbComplete;
    cbRemaining -= cbComplete;

    if (cbRemaining >= kBlockSize - 2 * sizeof(uint32_t)) {
        /* Two trailer blocks. */
        memcpy(&au8BlockBuffer[0],           pu8,          cbRemaining);
        memcpy(&au8BlockBuffer[cbRemaining], s_au8Padding, kBlockSize - cbRemaining);
        md5Update(&ctx, au8BlockBuffer, kBlockSize);
        memset(&au8BlockBuffer[sizeof(uint32_t)], 0, kBlockSize - 2 * sizeof(uint32_t));
    } else {
        /* One trailer block. */
        memcpy(&au8BlockBuffer[sizeof(uint32_t)], pu8, cbRemaining);
        memcpy(&au8BlockBuffer[sizeof(uint32_t) + cbRemaining],
               s_au8Padding, kBlockSize - cbRemaining - 2 * sizeof(uint32_t));
    }
    *(uint32_t *)&au8BlockBuffer[0]                         = (uint32_t)cbData << 3;
    *(uint32_t *)&au8BlockBuffer[kBlockSize - sizeof(uint32_t)] = ((uint32_t)cbData << 1) | 1u;
    md5Update(&ctx, au8BlockBuffer, kBlockSize);

    memcpy(pabDigest, ctx.buf, 16);
}

/* ---------- Byte writer ----------
 *
 * Append-only, pre-sized. Worst-case output for SM5 with 32 inputs +
 * 32 outputs + 32 patch consts is O(few KiB) of headers + cbTokens of
 * SHDR payload, all known at construction. */

typedef struct DxbcWriter {
    uint8_t *pBegin;
    uint8_t *pPtr;
    size_t   cbAlloc;
} DxbcWriter;

static int writerInit(DxbcWriter *w, size_t cbInitial)
{
    w->pBegin = (uint8_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cbInitial);
    if (!w->pBegin) return 0;
    w->pPtr     = w->pBegin;
    w->cbAlloc  = cbInitial;
    return 1;
}

static void writerFree(DxbcWriter *w)
{
    if (w->pBegin) HeapFree(GetProcessHeap(), 0, w->pBegin);
    w->pBegin = w->pPtr = NULL;
    w->cbAlloc = 0;
}

static size_t writerSize(const DxbcWriter *w) { return (size_t)(w->pPtr - w->pBegin); }

static int writerReserve(DxbcWriter *w, size_t cb)
{
    return (writerSize(w) + cb <= w->cbAlloc) ? 1 : 0;
}

static void *writerAppend(DxbcWriter *w, const void *p, size_t cb)
{
    if (!writerReserve(w, cb)) return NULL;
    void *dst = w->pPtr;
    if (p) memcpy(dst, p, cb); else memset(dst, 0, cb);
    w->pPtr += cb;
    return dst;
}

/* ---------- Semantic table ----------
 *
 * The DDI's D3D10_SB_NAME values (0..22) index this table to recover the
 * ASCIIZ name and DXBC enmSystemValue (D3D_NAME). Tess factor entries
 * collapse multiple D3D10_SB_NAME enum values onto a single D3D_NAME. */

typedef struct SemanticInfo {
    const char *name;
    uint32_t    compType;
    uint32_t    dxSysValue;
} SemanticInfo;

#define COMP_UINT32  1u
#define COMP_FLOAT32 3u

/* D3D_NAME values. */
#define D3D_SV_UNDEFINED                       0
#define D3D_SV_POSITION                        1
#define D3D_SV_CLIP_DISTANCE                   2
#define D3D_SV_CULL_DISTANCE                   3
#define D3D_SV_RENDER_TARGET_ARRAY_INDEX       4
#define D3D_SV_VIEWPORT_ARRAY_INDEX            5
#define D3D_SV_VERTEX_ID                       6
#define D3D_SV_PRIMITIVE_ID                    7
#define D3D_SV_INSTANCE_ID                     8
#define D3D_SV_IS_FRONT_FACE                   9
#define D3D_SV_SAMPLE_INDEX                    10
#define D3D_SV_FINAL_QUAD_EDGE_TESSFACTOR      11
#define D3D_SV_FINAL_QUAD_INSIDE_TESSFACTOR    12
#define D3D_SV_FINAL_TRI_EDGE_TESSFACTOR       13
#define D3D_SV_FINAL_TRI_INSIDE_TESSFACTOR     14
#define D3D_SV_FINAL_LINE_DETAIL_TESSFACTOR    15
#define D3D_SV_FINAL_LINE_DENSITY_TESSFACTOR   16

static const SemanticInfo g_semInfo[] = {
    /*  0 UNDEFINED              */ { "ATTRIB",                    COMP_FLOAT32, D3D_SV_UNDEFINED                    },
    /*  1 POSITION               */ { "SV_Position",               COMP_FLOAT32, D3D_SV_POSITION                     },
    /*  2 CLIP_DISTANCE          */ { "SV_ClipDistance",           COMP_FLOAT32, D3D_SV_CLIP_DISTANCE                },
    /*  3 CULL_DISTANCE          */ { "SV_CullDistance",           COMP_FLOAT32, D3D_SV_CULL_DISTANCE                },
    /*  4 RT_ARRAY_INDEX         */ { "SV_RenderTargetArrayIndex", COMP_UINT32,  D3D_SV_RENDER_TARGET_ARRAY_INDEX    },
    /*  5 VP_ARRAY_INDEX         */ { "SV_ViewportArrayIndex",     COMP_UINT32,  D3D_SV_VIEWPORT_ARRAY_INDEX         },
    /*  6 VERTEX_ID              */ { "SV_VertexID",               COMP_UINT32,  D3D_SV_VERTEX_ID                    },
    /*  7 PRIMITIVE_ID           */ { "SV_PrimitiveID",            COMP_UINT32,  D3D_SV_PRIMITIVE_ID                 },
    /*  8 INSTANCE_ID            */ { "SV_InstanceID",             COMP_UINT32,  D3D_SV_INSTANCE_ID                  },
    /*  9 IS_FRONT_FACE          */ { "SV_IsFrontFace",            COMP_UINT32,  D3D_SV_IS_FRONT_FACE                },
    /* 10 SAMPLE_INDEX           */ { "SV_SampleIndex",            COMP_UINT32,  D3D_SV_SAMPLE_INDEX                 },
    /* 11 QUAD_U0_EDGE_TESS      */ { "SV_TessFactor",             COMP_FLOAT32, D3D_SV_FINAL_QUAD_EDGE_TESSFACTOR   },
    /* 12 QUAD_V0_EDGE_TESS      */ { "SV_TessFactor",             COMP_FLOAT32, D3D_SV_FINAL_QUAD_EDGE_TESSFACTOR   },
    /* 13 QUAD_U1_EDGE_TESS      */ { "SV_TessFactor",             COMP_FLOAT32, D3D_SV_FINAL_QUAD_EDGE_TESSFACTOR   },
    /* 14 QUAD_V1_EDGE_TESS      */ { "SV_TessFactor",             COMP_FLOAT32, D3D_SV_FINAL_QUAD_EDGE_TESSFACTOR   },
    /* 15 QUAD_U_INSIDE_TESS     */ { "SV_InsideTessFactor",       COMP_FLOAT32, D3D_SV_FINAL_QUAD_INSIDE_TESSFACTOR },
    /* 16 QUAD_V_INSIDE_TESS     */ { "SV_InsideTessFactor",       COMP_FLOAT32, D3D_SV_FINAL_QUAD_INSIDE_TESSFACTOR },
    /* 17 TRI_U0_EDGE_TESS       */ { "SV_TessFactor",             COMP_FLOAT32, D3D_SV_FINAL_TRI_EDGE_TESSFACTOR    },
    /* 18 TRI_V0_EDGE_TESS       */ { "SV_TessFactor",             COMP_FLOAT32, D3D_SV_FINAL_TRI_EDGE_TESSFACTOR    },
    /* 19 TRI_W0_EDGE_TESS       */ { "SV_TessFactor",             COMP_FLOAT32, D3D_SV_FINAL_TRI_EDGE_TESSFACTOR    },
    /* 20 TRI_INSIDE_TESS        */ { "SV_InsideTessFactor",       COMP_FLOAT32, D3D_SV_FINAL_TRI_INSIDE_TESSFACTOR  },
    /* 21 LINE_DETAIL_TESS       */ { "SV_TessFactor",             COMP_FLOAT32, D3D_SV_FINAL_LINE_DETAIL_TESSFACTOR },
    /* 22 LINE_DENSITY_TESS      */ { "SV_TessFactor",             COMP_FLOAT32, D3D_SV_FINAL_LINE_DENSITY_TESSFACTOR},
};

#define G_SEM_INFO_COUNT ((UINT)(sizeof(g_semInfo) / sizeof(g_semInfo[0])))

/* PS-output UNDEFINED → "SV_Target". */
static const SemanticInfo g_semPSOutput = { "SV_Target", COMP_FLOAT32, D3D_SV_UNDEFINED };

/* Program type extracted from VGPU10ProgramToken (= pTokens[0]). The
 * upper 16 bits hold programType: 0=PS, 1=VS, 2=GS, 3=HS, 4=DS, 5=CS. */
enum {
    PROG_PS = 0,
    PROG_VS = 1,
    PROG_GS = 2,
    PROG_HS = 3,
    PROG_DS = 4,
    PROG_CS = 5,
};

static UINT programType(const UINT *pTokens)
{
    return (pTokens[0] >> 16) & 0xFFFFu;
}

/* ---------- SM4/5 token-stream iterator ----------
 *
 * pTokens[0] is the version token, pTokens[1] the total length in DWORDs,
 * instructions follow.  An instruction's DWORD count normally lives in
 * OpcodeToken0[30:24]; encodings with a zero length field (CUSTOMDATA,
 * SM5 interface tables) carry the total count in the DWORD after the
 * opcode token instead. */

typedef struct TritonTokIter {
    const UINT *p;
    const UINT *end;
} TritonTokIter;

static int tritonTokIterInit(TritonTokIter *it, const UINT *pTokens, SIZE_T cbTokens)
{
    if (!pTokens || cbTokens < 3 * sizeof(UINT))
        return 0;
    const UINT *end = pTokens + pTokens[1];
    const UINT *cap = pTokens + (cbTokens / sizeof(UINT));
    it->end = (end > cap) ? cap : end;
    it->p   = pTokens + 2;
    return 1;
}

/* Yields the instruction at *ppIns spanning *pcTok DWORDs and advances past
 * it; returns 0 at end of stream or on a malformed encoding. */
static int tritonTokIterNext(TritonTokIter *it, UINT *pOpcode,
                             const UINT **ppIns, UINT *pcTok)
{
    if (it->p >= it->end)
        return 0;
    const UINT tok = it->p[0];
    UINT cTok = (tok >> 24) & 0x7fu;
    if (cTok == 0) {
        if (it->end - it->p < 2)
            return 0;
        cTok = it->p[1];
        if (cTok < 2)
            return 0;
    }
    if ((SIZE_T)(it->end - it->p) < cTok)
        return 0;
    *pOpcode = tok & 0x7ffu;
    *ppIns   = it->p;
    *pcTok   = cTok;
    it->p += cTok;
    return 1;
}

/* D3D10_SB_OPCODE_TYPE values consumed here. */
#define SM4_OP_DCL_INPUT              0x5fu
#define SM4_OP_DCL_INPUT_SGV          0x60u
#define SM4_OP_DCL_INPUT_SIV          0x61u
#define SM4_OP_DCL_INPUT_PS           0x62u
#define SM4_OP_DCL_INPUT_PS_SGV       0x63u
#define SM4_OP_DCL_INPUT_PS_SIV       0x64u
#define SM4_OP_DCL_OUTPUT             0x65u
#define SM4_OP_DCL_OUTPUT_SGV         0x66u
#define SM4_OP_DCL_OUTPUT_SIV         0x67u
#define SM5_OP_HS_CONTROL_POINT_PHASE 0x72u
#define SM5_OP_HS_FORK_PHASE          0x73u
#define SM5_OP_HS_JOIN_PHASE          0x74u
#define SM5_OP_DCL_STREAM             0x8fu

/* D3D10_SB_OPERAND_TYPE values consumed here. */
#define SM4_OPTYPE_INPUT               1u
#define SM4_OPTYPE_OUTPUT              2u
#define SM4_OPTYPE_INPUT_PRIMITIVEID   11u
#define SM4_OPTYPE_OUTPUT_DEPTH        12u
#define SM4_OPTYPE_OUTPUT_COVERAGE     15u
#define SM4_OPTYPE_STREAM              16u
#define SM4_OPTYPE_INPUT_CONTROL_POINT 25u
#define SM4_OPTYPE_INPUT_PATCH_CONST   27u
#define SM4_OPTYPE_OUTPUT_DEPTH_GE     38u
#define SM4_OPTYPE_OUTPUT_DEPTH_LE     39u
#define SM4_OPTYPE_OUTPUT_STENCIL_REF  41u

/* ---------- DDI signature entry reader ----------
 *
 * D3D10DDIARG_SIGNATURE_ENTRY (12 B) and D3D11_1DDIARG_SIGNATURE_ENTRY
 * (20 B) start with the same 12-byte head: SystemValue (4), Register
 * (4), Mask (1 + 3 B pad). The 20-byte form appends
 * RegisterComponentType (4) and MinPrecision (4);
 * D3D11_1DDIARG_SIGNATURE_ENTRY2 additionally carries a Stream byte in
 * the padding after Mask. The Vista D3D9 bridge supplies the normalized
 * TRITON_DXBC_SIGNATURE form when semantic names are available. */

typedef TRITON_DXBC_SIGNATURE DdiSig;

/* ---------- IOSGN blob writer ----------
 *
 * Layout in memory:
 *   [DXBCBlobHeader]
 *   [DXBCBlobIOSGN  (cElement, offElement=8)]
 *   [DXBCBlobIOSGNElement * cElement]
 *   [name strings, ASCIIZ, packed]
 *   [0..3 bytes of zero padding to 4-byte alignment]
 * The blob's cbBlob field counts everything after the BlobHeader
 * (including padding). offElementName is relative to the start of the
 * DXBCBlobIOSGN (i.e. excludes the 8-byte BlobHeader).
 *
 * SemanticIndex is auto-numbered per name in signature order (ATTRIB0,
 * ATTRIB1, …; SV_TessFactor0..3), except SV_Target, whose index is the
 * render-target register.  The shader-side input layout resolver reads
 * these names back. */

static const SemanticInfo *pickSemanticInfo(UINT systemValue, UINT blobType,
                                            UINT progType)
{
    if (systemValue == 0
        && blobType   == DXBC_BLOB_TYPE_OSGN
        && progType   == PROG_PS)
        return &g_semPSOutput;

    if (systemValue < G_SEM_INFO_COUNT)
        return &g_semInfo[systemValue];

    return &g_semInfo[0]; /* unknown SystemValue → ATTRIB */
}

/* Per-blob entry cap. Signature ELEMENTS can outnumber the 32 registers of
 * a register class: packing places up to 4 scalar semantics in one register,
 * so a fully packed class is 128 elements. */
#define TRITON_DXBC_MAX_SIG_ENTRIES 128u

/* ---------- SHDR declaration scan ----------
 *
 * Collects every input/output/patch-constant register the shader declares,
 * plus the pixel-shader system outputs that occupy no o# register
 * (depth/coverage/stencil).  This drives two things:
 *   - signature completeness: the runtime can hand the DDI FEWER signature
 *     entries than the shader declares (a stream-output GS reports none at
 *     all), and a signature element missing for a live register makes
 *     D3DMetal's libdxilconv NULL-deref while resolving the operand
 *     (LoadOperand / StoreOperand -> DxilSignatureElement::GetCompType);
 *   - naming the register-less PS system outputs, whose DDI entries carry
 *     SystemValue 0 and would otherwise be mislabeled "SV_Target".
 */

/* Decode one operand starting at op[0] (the operand token).  Returns the
 * operand's length in dwords (>=1) or 0 if it can't be decoded (relative
 * indexing -- never used in a plain declaration).  Fills the register (last
 * index), the write/use mask, and the operand type. */
static UINT
tritonDecodeDeclOperand(const UINT *op, const UINT *end,
                        UINT *outReg, UINT *outMask, UINT *outType)
{
    if (op + 1 > end)
        return 0;
    const UINT ot = op[0];
    UINT len = 1;

    /* Extended operand token chain (bit 31 -> another token follows). */
    if (ot & 0x80000000u) {
        while (op + len <= end && (op[len - 1] & 0x80000000u))
            ++len;
    }

    /* Write mask: 4-component operands in mask-selection mode (bits [3:2]==0)
     * carry a 4-bit mask in bits [7:4]; otherwise treat as all components. */
    UINT mask = 0xFu;
    if ((ot & 0x3u) == 2u && ((ot >> 2) & 0x3u) == 0u)
        mask = (ot >> 4) & 0xFu;

    /* Walk the index dwords; the register number is the LAST index. */
    const UINT dim = (ot >> 20) & 0x3u;
    UINT reg = 0;
    for (UINT d = 0; d < dim; ++d) {
        const UINT rep = (ot >> (22u + 3u * d)) & 0x7u;
        if (op + len >= end)
            return 0;
        if (rep == 0u) {                 /* IMMEDIATE32 (1 dword) */
            reg = op[len]; len += 1;
        } else if (rep == 1u) {          /* IMMEDIATE64 (2 dwords) */
            if (op + len + 1 >= end)
                return 0;
            reg = op[len]; len += 2;
        } else {                         /* RELATIVE / *_PLUS_RELATIVE */
            return 0;                    /* not used in declarations */
        }
    }

    *outReg  = reg;
    *outMask = mask;
    *outType = (ot >> 12) & 0xffu;
    return len;
}

typedef struct TritonDeclSig {
    UINT reg;
    UINT mask;
    UINT sysval;    /* D3D10_SB_NAME from _SGV/_SIV declarations, else 0 */
    UINT stream;    /* GS output stream, else 0 */
} TritonDeclSig;

typedef struct TritonShdrScan {
    TritonDeclSig aIn[TRITON_DXBC_MAX_SIG_ENTRIES];    UINT cIn;
    TritonDeclSig aOut[TRITON_DXBC_MAX_SIG_ENTRIES];   UINT cOut;
    TritonDeclSig aPatch[TRITON_DXBC_MAX_SIG_ENTRIES]; UINT cPatch;
    UINT aSysOut[4];    /* PS o#-less output operand types, decl order */
    UINT cSysOut;
    UINT cStream;       /* 1 + highest declared GS stream */
} TritonShdrScan;

static void tritonDeclAdd(TritonDeclSig *pa, UINT *pc,
                          UINT reg, UINT mask, UINT sysval, UINT stream)
{
    /* Merge only overlapping-component redeclarations (HS phases redeclare
     * the same components).  Disjoint declarations sharing a register are
     * SEPARATE packed signature elements (fxc emits one dcl per element,
     * e.g. dcl o2.xyz + dcl o2.w) and must stay split: the converter links
     * inter-stage attributes per element (register + start component), so
     * a merged producer element never matches the consumer's split ones. */
    for (UINT i = 0; i < *pc; ++i) {
        if (pa[i].reg == reg && pa[i].stream == stream
            && ((pa[i].mask & mask) || !mask || !pa[i].mask)) {
            pa[i].mask |= mask;
            if (sysval && !pa[i].sysval)
                pa[i].sysval = sysval;
            return;
        }
    }
    if (*pc < TRITON_DXBC_MAX_SIG_ENTRIES) {
        pa[*pc].reg    = reg;
        pa[*pc].mask   = mask;
        pa[*pc].sysval = sysval;
        pa[*pc].stream = stream;
        ++*pc;
    }
}

static void tritonScanShdrDecls(const UINT *pTokens, SIZE_T cbTokens,
                                TritonShdrScan *s)
{
    memset(s, 0, sizeof(*s));
    s->cStream = 1;

    TritonTokIter it;
    if (!tritonTokIterInit(&it, pTokens, cbTokens))
        return;

    const UINT progType = programType(pTokens);
    UINT stream = 0;
    int fPatchPhase = 0;    /* HS fork/join dcl_output writes patch constants */

    UINT opcode, cTok;
    const UINT *ins;
    while (tritonTokIterNext(&it, &opcode, &ins, &cTok)) {
        switch (opcode) {
        case SM5_OP_HS_CONTROL_POINT_PHASE:
            fPatchPhase = 0;
            continue;
        case SM5_OP_HS_FORK_PHASE:
        case SM5_OP_HS_JOIN_PHASE:
            fPatchPhase = 1;
            continue;
        case SM5_OP_DCL_STREAM: {
            UINT reg = 0, mask = 0, optype = 0;
            if (tritonDecodeDeclOperand(ins + 1, ins + cTok, &reg, &mask, &optype)
                && optype == SM4_OPTYPE_STREAM && reg < 4u) {
                stream = reg;
                if (stream + 1 > s->cStream)
                    s->cStream = stream + 1;
            }
            continue;
        }
        default:
            break;
        }

        if (opcode < SM4_OP_DCL_INPUT || opcode > SM4_OP_DCL_OUTPUT_SIV)
            continue;

        UINT reg = 0, mask = 0, optype = 0;
        UINT cOp = tritonDecodeDeclOperand(ins + 1, ins + cTok, &reg, &mask, &optype);
        if (!cOp)
            continue;

        const int fCarriesName = (opcode == SM4_OP_DCL_INPUT_SGV    ||
                                  opcode == SM4_OP_DCL_INPUT_SIV    ||
                                  opcode == SM4_OP_DCL_INPUT_PS_SGV ||
                                  opcode == SM4_OP_DCL_INPUT_PS_SIV ||
                                  opcode == SM4_OP_DCL_OUTPUT_SGV   ||
                                  opcode == SM4_OP_DCL_OUTPUT_SIV);
        UINT sysval = 0;
        if (fCarriesName && 1 + cOp < cTok)
            sysval = ins[1 + cOp] & 0xffffu;    /* trailing D3D_NAME token */

        if (opcode <= SM4_OP_DCL_INPUT_PS_SIV) {
            /* v# / vicp# registers form the input signature; vpc# the patch-
             * constant one (DS).  A GS's vPrim carries a register-less ISGN
             * element (SV_PrimitiveID, Register 0xFFFFFFFF).  The remaining
             * system registers (vCoverage, thread IDs, ...) have no signature
             * element and are skipped. */
            if (optype == SM4_OPTYPE_INPUT ||
                optype == SM4_OPTYPE_INPUT_CONTROL_POINT)
                tritonDeclAdd(s->aIn, &s->cIn, reg, mask, sysval, 0);
            else if (optype == SM4_OPTYPE_INPUT_PATCH_CONST)
                tritonDeclAdd(s->aPatch, &s->cPatch, reg, mask, sysval, 0);
            else if (optype == SM4_OPTYPE_INPUT_PRIMITIVEID
                     && progType == PROG_GS)
                tritonDeclAdd(s->aIn, &s->cIn, 0xFFFFFFFFu, 0x1u,
                              7u /* D3D10_SB_NAME_PRIMITIVE_ID */, 0);
        } else {
            if (optype == SM4_OPTYPE_OUTPUT) {
                if (fPatchPhase)
                    tritonDeclAdd(s->aPatch, &s->cPatch, reg, mask, sysval, 0);
                else
                    tritonDeclAdd(s->aOut, &s->cOut, reg, mask, sysval,
                                  progType == PROG_GS ? stream : 0);
            } else if (progType == PROG_PS
                       && s->cSysOut < (UINT)(sizeof(s->aSysOut) / sizeof(s->aSysOut[0]))
                       && (optype == SM4_OPTYPE_OUTPUT_DEPTH       ||
                           optype == SM4_OPTYPE_OUTPUT_COVERAGE    ||
                           optype == SM4_OPTYPE_OUTPUT_DEPTH_GE    ||
                           optype == SM4_OPTYPE_OUTPUT_DEPTH_LE    ||
                           optype == SM4_OPTYPE_OUTPUT_STENCIL_REF)) {
                s->aSysOut[s->cSysOut++] = optype;
            }
        }
    }
}

/* ---------- Signature entry collection ----------
 *
 * Internal form each signature blob is built from: the DDI entries first,
 * then entries synthesised for declared registers the DDI set lacks, the
 * whole set sorted by (stream, register, first component).  The sort keeps
 * packed elements sharing a register in runtime order and matches the
 * register-ascending layout fxc emits, which converters rely on when
 * merging indexed ranges. */

typedef struct TritonSigEntry {
    UINT sysval;        /* D3D10_SB_NAME */
    UINT reg;
    UINT mask;
    UINT compType;      /* D3D10_SB_REGISTER_COMPONENT_TYPE; 0 = per-semantic */
    UINT minPrec;       /* D3D11_SB_OPERAND_MIN_PRECISION */
    UINT stream;
    const char *name;   /* resolved by tritonAssignSemantics */
    UINT dxSysValue;    /* resolved D3D_NAME for the blob */
    UINT semIdx;        /* 0xFFFFFFFF until resolved */
} TritonSigEntry;

static UINT tritonFirstComp(UINT mask)
{
    for (UINT c = 0; c < 4; ++c)
        if (mask & (1u << c))
            return c;
    return 0;
}

static int tritonSigOrderBefore(const TritonSigEntry *a, const TritonSigEntry *b)
{
    if (a->stream != b->stream)
        return a->stream < b->stream;
    if (a->reg != b->reg)
        return a->reg < b->reg;     /* register-less (0xFFFFFFFF) sorts last */
    return tritonFirstComp(a->mask) < tritonFirstComp(b->mask);
}

static UINT tritonCollectSigEntries(const void *pDdi, UINT cDdi, UINT stride,
                                    const TritonDeclSig *pDecl, UINT cDecl,
                                    UINT cStream, TritonSigEntry *pOut)
{
    UINT n = 0;

    if (cDdi > TRITON_DXBC_MAX_SIG_ENTRIES) {
        TR_LOG("tritonCollectSigEntries: truncating %u DDI entries to %u",
               cDdi, TRITON_DXBC_MAX_SIG_ENTRIES);
        cDdi = TRITON_DXBC_MAX_SIG_ENTRIES;
    }

    for (UINT i = 0; i < cDdi; ++i) {
        DdiSig d;
        if (!tritonDxbcSignatureFromDdi(pDdi, stride, i, &d))
            continue;
        TritonSigEntry *e = &pOut[n++];
        e->sysval     = d.systemValue;
        e->reg        = d.registerIdx;
        e->mask       = (UINT)(d.mask & 0x0Fu);
        e->compType   = d.componentType;
        e->minPrec    = d.minPrecision;
        e->stream     = 0;
        e->name       = d.semanticName;
        e->dxSysValue = 0;
        e->semIdx     = d.semanticName ? d.semanticIndex : 0xFFFFFFFFu;
        /* Multi-stream signatures need every entry attributed to a stream,
         * but the ENTRY2 stream byte occupies what older runtimes treated as
         * padding (and the 12-byte form has none at all).  Keep a DDI entry
         * only when the shader's own declarations corroborate the (stream,
         * register) pair; dropped entries are re-synthesised from the
         * declarations below with the correct stream. */
        if (cStream > 1) {
            int fCorroborated = 0;
            if (d.stream < cStream) {
                for (UINT j = 0; j < cDecl; ++j) {
                    if (pDecl[j].stream == d.stream
                        && pDecl[j].reg == d.registerIdx) {
                        fCorroborated = 1;
                        break;
                    }
                }
            }
            if (!fCorroborated) {
                --n;
                continue;
            }
            e->stream = d.stream;
            /* A stream-less DDI array reports a register shared by several
             * streams once per stream; after attribution those repeats
             * collide on (stream, register) with overlapping masks (packed
             * pairs stay: their masks are disjoint). */
            int fDup = 0;
            for (UINT j = 0; j + 1 < n; ++j) {
                if (pOut[j].stream == e->stream && pOut[j].reg == e->reg
                    && (pOut[j].mask & e->mask)) {
                    fDup = 1;
                    break;
                }
            }
            if (fDup) {
                --n;
                continue;
            }
        }
    }

    /* Synthesise entries for declared elements the DDI set lacks.  Match
     * on component overlap, not just register: packed elements share a
     * register (dcl o2.xyz + dcl o2.w are two elements), and an existing
     * entry for one must not suppress synthesis of the other. */
    for (UINT i = 0; i < cDecl && n < TRITON_DXBC_MAX_SIG_ENTRIES; ++i) {
        UINT j;
        for (j = 0; j < n; ++j)
            if (pOut[j].reg == pDecl[i].reg && pOut[j].stream == pDecl[i].stream
                && ((pOut[j].mask & pDecl[i].mask) || !pDecl[i].mask
                    || !pOut[j].mask))
                break;
        if (j < n)
            continue;
        /* Recover the component type and min-precision from the DDI entry for
         * this register.  The entry was dropped over a stream disagreement, not
         * a type one, and a stream-less DDI array repeats the register per
         * stream with the same type -- so matching on register (plus component
         * overlap for packed elements) restores what synthesis would otherwise
         * lose.  Without this a uint/int output comes back typed float32 from
         * the semantic table, and the lost min-precision silently demotes the
         * signature to its non-min-precision chunk variant. */
        UINT synthCompType = 0, synthMinPrec = 0;
        for (UINT k = 0; k < cDdi; ++k) {
            DdiSig d;
            if (!tritonDxbcSignatureFromDdi(pDdi, stride, k, &d))
                continue;
            if (d.registerIdx != pDecl[i].reg)
                continue;
            if (pDecl[i].mask && (d.mask & 0x0Fu) &&
                !((d.mask & 0x0Fu) & pDecl[i].mask))
                continue;
            synthCompType = d.componentType;
            synthMinPrec  = d.minPrecision;
            break;
        }
        TritonSigEntry *e = &pOut[n++];
        e->sysval     = pDecl[i].sysval;
        e->reg        = pDecl[i].reg;
        e->mask       = pDecl[i].mask & 0x0Fu;
        e->compType   = synthCompType;
        e->minPrec    = synthMinPrec;
        e->stream     = pDecl[i].stream;
        e->name       = NULL;
        e->dxSysValue = 0;
        e->semIdx     = 0xFFFFFFFFu;
    }

    /* Stable insertion sort by (stream, register, first component). */
    for (UINT i = 1; i < n; ++i) {
        TritonSigEntry key = pOut[i];
        UINT j = i;
        while (j > 0 && tritonSigOrderBefore(&key, &pOut[j - 1])) {
            pOut[j] = pOut[j - 1];
            --j;
        }
        pOut[j] = key;
    }
    return n;
}

/* ---------- Semantic resolution ---------- */

static const char *tritonSysOutName(UINT optype)
{
    switch (optype) {
    case SM4_OPTYPE_OUTPUT_DEPTH:       return "SV_Depth";
    case SM4_OPTYPE_OUTPUT_COVERAGE:    return "SV_Coverage";
    case SM4_OPTYPE_OUTPUT_DEPTH_GE:    return "SV_DepthGreaterEqual";
    case SM4_OPTYPE_OUTPUT_DEPTH_LE:    return "SV_DepthLessEqual";
    case SM4_OPTYPE_OUTPUT_STENCIL_REF: return "SV_StencilRef";
    default:                            return NULL;
    }
}

static UINT tritonSysOutCompType(UINT optype)
{
    return (optype == SM4_OPTYPE_OUTPUT_COVERAGE ||
            optype == SM4_OPTYPE_OUTPUT_STENCIL_REF) ? COMP_UINT32
                                                     : COMP_FLOAT32;
}

/* Resolves name / D3D_NAME / component type / semantic index for every
 * entry, and appends entries for declared PS system outputs the DDI set
 * lacks.  Returns the final entry count. */
static UINT tritonAssignSemantics(TritonSigEntry *a, UINT n,
                                  UINT blobType, UINT progType,
                                  const TritonShdrScan *scan)
{
    const int fPSOut = (blobType == DXBC_BLOB_TYPE_OSGN && progType == PROG_PS);
    UINT claimed = 0;   /* bitmask over scan->aSysOut */

    for (UINT i = 0; i < n; ++i) {
        TritonSigEntry *e = &a[i];
        const SemanticInfo *info = pickSemanticInfo(e->sysval, blobType, progType);
        if (!e->name)
            e->name = info->name;
        e->dxSysValue = e->sysval ? info->dxSysValue : 0;

        if (fPSOut && info == &g_semPSOutput) {
            if (e->reg != 0xFFFFFFFFu) {
                /* SV_Target's semantic index IS the render-target register:
                 * o2 must be SV_Target2 (DXIL validation: "SV_Target semantic
                 * index must match packed row location").  Counting same-named
                 * entries instead misroutes color output for sparse or
                 * reordered MRT sets on hosts that bind targets by index. */
                e->semIdx = e->reg;
            } else {
                /* Depth/coverage/stencil outputs occupy no o# register and
                 * reach the DDI with SystemValue 0; libdxilconv resolves their
                 * stores by NAME, so recover it from the shader's system-
                 * output declarations.  Prefer a component-type match
                 * (coverage/stencil are uint, depth float) so signature order
                 * need not mirror declaration order. */
                int wantUint = -1;
                if (e->compType == COMP_UINT32 || e->compType == 2u)
                    wantUint = 1;
                else if (e->compType == COMP_FLOAT32)
                    wantUint = 0;
                UINT pick = 0xFFFFFFFFu;
                for (UINT j = 0; j < scan->cSysOut; ++j) {
                    if (claimed & (1u << j))
                        continue;
                    const int fUint =
                        (tritonSysOutCompType(scan->aSysOut[j]) == COMP_UINT32);
                    if (wantUint < 0 || fUint == wantUint) {
                        pick = j;
                        break;
                    }
                    if (pick == 0xFFFFFFFFu)
                        pick = j;
                }
                if (pick != 0xFFFFFFFFu) {
                    claimed |= 1u << pick;
                    e->name     = tritonSysOutName(scan->aSysOut[pick]);
                    e->compType = tritonSysOutCompType(scan->aSysOut[pick]);
                    if (!e->mask)
                        e->mask = 0x1u;
                    e->semIdx = 0;
                }
            }
        }

        if (!e->compType)
            e->compType = info->compType;
    }

    /* Declared PS system outputs with no DDI entry at all: append one, or
     * libdxilconv NULL-derefs storing to it. */
    if (fPSOut) {
        for (UINT j = 0; j < scan->cSysOut && n < TRITON_DXBC_MAX_SIG_ENTRIES; ++j) {
            if (claimed & (1u << j))
                continue;
            TritonSigEntry *e = &a[n++];
            e->sysval     = 0;
            e->reg        = 0xFFFFFFFFu;
            e->mask       = 0x1u;
            e->compType   = tritonSysOutCompType(scan->aSysOut[j]);
            e->minPrec    = 0;
            e->stream     = 0;
            e->name       = tritonSysOutName(scan->aSysOut[j]);
            e->dxSysValue = 0;
            e->semIdx     = 0;
        }
    }

    /* Sequential per-name numbering for everything not fixed above, in
     * signature order: ATTRIB0..N, SV_TessFactor0..3, SV_ClipDistance0/1.
     * Numbering is scoped per stream: each GS stream is its own signature
     * namespace (its SV_Position is SV_Position0 again). */
    for (UINT i = 0; i < n; ++i) {
        if (a[i].semIdx != 0xFFFFFFFFu)
            continue;
        UINT idx = 0;
        for (UINT j = 0; j < i; ++j)
            if (a[j].stream == a[i].stream && strcmp(a[j].name, a[i].name) == 0)
                ++idx;
        a[i].semIdx = idx;
    }
    return n;
}

/* ---------- Signature blob writer ----------
 *
 * Element layouts by chunk variant (byte offsets within one element):
 *                   Stream NameOff SemIdx SysVal CompType Register Mask/Used MinPrec
 *   ISGN/OSGN/PCSG    -      0       4      8      12       16       20        -
 *   OSG5              0      4       8     12      16       20       24        -
 *   ISG1/OSG1/PSG1    0      4       8     12      16       20       24       28
 */

static void tritonSigBlobLayout(UINT fourcc, UINT *pcbEntry,
                                int *pfStream, int *pfMinPrec)
{
    switch (fourcc) {
    case DXBC_BLOB_TYPE_OSG5:
        *pcbEntry = 28; *pfStream = 1; *pfMinPrec = 0;
        break;
    case DXBC_BLOB_TYPE_ISG1:
    case DXBC_BLOB_TYPE_OSG1:
    case DXBC_BLOB_TYPE_PSG1:
        *pcbEntry = 32; *pfStream = 1; *pfMinPrec = 1;
        break;
    default:
        *pcbEntry = 24; *pfStream = 0; *pfMinPrec = 0;
        break;
    }
}

/* blobClass is DXBC_BLOB_TYPE_ISGN / _OSGN / _PCSG and selects the
 * class-specific rules (Used mask semantics, VS input widening); fourcc is
 * the chunk actually emitted for that class. */
static int writeSigBlob(DxbcWriter *w, UINT fourcc, UINT blobClass,
                        UINT progType, const TritonSigEntry *pEntries,
                        UINT cEntries)
{
    if (cEntries > TRITON_DXBC_MAX_SIG_ENTRIES) {
        TR_LOG("writeSigBlob: cEntries=%u exceeds cap %u",
               cEntries, TRITON_DXBC_MAX_SIG_ENTRIES);
        return 0;
    }

    UINT cbEntry;
    int fStream, fMinPrec;
    tritonSigBlobLayout(fourcc, &cbEntry, &fStream, &fMinPrec);
    const UINT offBase = fStream ? 4u : 0u;

    DXBCBlobHeader *pHdr = (DXBCBlobHeader *)writerAppend(w, NULL, sizeof(DXBCBlobHeader));
    if (!pHdr) return 0;
    pHdr->u32BlobType = fourcc;
    pHdr->cbBlob = 0; /* patched at the end */

    DXBCBlobIOSGN *pIO = (DXBCBlobIOSGN *)writerAppend(w, NULL, sizeof(DXBCBlobIOSGN));
    if (!pIO) return 0;
    pIO->cElement   = cEntries;
    pIO->offElement = 8;

    BYTE *pElems = (BYTE *)writerAppend(w, NULL, (size_t)cEntries * cbEntry);
    if (cEntries && !pElems) return 0;

    /* Name-offset slot of element i, patched when names are emitted. */
#define SIG_NAME_SLOT(i) ((UINT *)(pElems + (size_t)(i) * cbEntry + offBase))

    for (UINT i = 0; i < cEntries; ++i) {
        const TritonSigEntry *s = &pEntries[i];
        BYTE *e = pElems + (size_t)i * cbEntry;
        if (fStream)
            *(UINT *)(e + 0) = s->stream;
        *(UINT *)(e + offBase + 0)  = 0;             /* name offset, patched */
        *(UINT *)(e + offBase + 4)  = s->semIdx;
        *(UINT *)(e + offBase + 8)  = s->dxSysValue;
        *(UINT *)(e + offBase + 12) = s->compType;
        *(UINT *)(e + offBase + 16) = s->reg;

        UINT m = s->mask & 0x0Fu;
        /* D3DMetal's libmetalirconverter SIGSEGVs at pipeline compile (crash in
         * llvm::TypeFinder under the cross-module type linker) on a VS input
         * declared with a single component. Widen scalar IA-fed inputs so the
         * compile takes the working multi-component fetch path. The mask here
         * is a USED-component mask whose bits are absolute component indices,
         * so extend from .x up to the declared component rather than shifting
         * it down: .x/.y -> 0x3, .z -> 0x7, .w -> 0xF. The added components
         * read as zero and are harmless. System-value inputs (SV_VertexID etc.,
         * enmSystemValue != 0) are excluded -- widening those corrupts the
         * shader -- and that same guard keeps the widen off packed signatures,
         * so it can never clobber a neighbour: IA inputs occupy one element per
         * register. There is no input layout at VS build time, so this applies
         * to every single-component IA input regardless of the vertex format
         * later bound. */
        if (progType == PROG_VS && blobClass == DXBC_BLOB_TYPE_ISGN
            && s->dxSysValue == 0 && m && (m & (m - 1)) == 0
            && (npt_host_workaround_flags() & NPT_WA_WIDEN_SCALAR_VS_INPUT_MASK))
            m = ((m << 1) - 1u) | 0x3u;
        UINT used;
        if (blobClass == DXBC_BLOB_TYPE_ISGN) {
            /* dxvk requires Used == Mask for input layout validation. */
            used = m;
        } else if (blobClass == DXBC_BLOB_TYPE_OSGN) {
            /* NeverWrites: bits set = components NOT written. We declare
             * "all written" → NeverWrites = ~Mask & 0xF. */
            used = (~m) & 0x0Fu;
        } else {
            /* PCSG: patch constants are outputs of the HS (NeverWrites,
             * declare all written) and inputs of the DS (AlwaysReads,
             * declare all read — under-declaring invites dead-code
             * elimination of live patch-constant loads). */
            used = (progType == PROG_HS) ? ((~m) & 0x0Fu) : m;
        }
        *(UINT *)(e + offBase + 20) = (m & 0xFFu) | ((used & 0xFFu) << 8);

        if (fMinPrec)
            *(UINT *)(e + 28) = s->minPrec;
    }

    /* Emit ASCIIZ names with dedup by string equality. */
    for (UINT i = 0; i < cEntries; ++i) {
        if (*SIG_NAME_SLOT(i) != 0) continue;
        UINT off = (UINT)(writerSize(w) - ((const uint8_t *)pIO - w->pBegin));
        size_t cb = strlen(pEntries[i].name) + 1;
        if (!writerAppend(w, pEntries[i].name, cb)) return 0;
        *SIG_NAME_SLOT(i) = off;
        for (UINT j = i + 1; j < cEntries; ++j) {
            if (*SIG_NAME_SLOT(j) == 0
                && strcmp(pEntries[j].name, pEntries[i].name) == 0)
                *SIG_NAME_SLOT(j) = off;
        }
    }
#undef SIG_NAME_SLOT

    /* Pad blob to 4-byte alignment. */
    size_t cbBody = writerSize(w) - ((const uint8_t *)pIO - w->pBegin);
    size_t pad = (4 - (cbBody & 3)) & 3;
    if (pad) {
        if (!writerAppend(w, NULL, pad)) return 0;
        cbBody += pad;
    }
    pHdr->cbBlob = (uint32_t)cbBody;
    return 1;
}

/* ---------- SHDR blob writer ----------
 * Verbatim passthrough of the token stream into a chunk payload, padded
 * to 4. The token stream is already aligned (uint32_t), so pad is
 * always 0 in practice.
 *
 * One normalization: noperspective interpolation modes in PS input
 * declarations are rewritten to their perspective-correct (linear)
 * counterparts. D3DMetal's shader converter (libmetalirconverter)
 * deterministically SIGSEGVs converting dcl_input_ps with
 * LINEAR_NOPERSPECTIVE (crash in DXIL2AIR's cross-module type linker).
 * The rewrite is unconditional: where w == 1 the two modes interpolate
 * identically, and for perspective-projected geometry the resulting incorrect
 * interpolation is preferable to losing the device. */

#define SM4_INTERP_SHIFT             11
#define SM4_INTERP_MASK              0xfu

static void patchNoPerspectiveDecls(UINT *pTok, SIZE_T cbTok)
{
    TritonTokIter it;
    if (!tritonTokIterInit(&it, pTok, cbTok))
        return;

    UINT patched = 0;
    UINT opcode, cTok;
    const UINT *ins;
    while (tritonTokIterNext(&it, &opcode, &ins, &cTok)) {
        if (opcode != SM4_OP_DCL_INPUT_PS &&
            opcode != SM4_OP_DCL_INPUT_PS_SGV &&
            opcode != SM4_OP_DCL_INPUT_PS_SIV)
            continue;
        const UINT t    = ins[0];
        const UINT mode = (t >> SM4_INTERP_SHIFT) & SM4_INTERP_MASK;
        /* 4 LINEAR_NOPERSPECTIVE -> 2 LINEAR
         * 5 LINEAR_NOPERSPECTIVE_CENTROID -> 3 LINEAR_CENTROID
         * 7 LINEAR_NOPERSPECTIVE_SAMPLE -> 6 LINEAR_SAMPLE */
        UINT newMode = mode;
        if (mode == 4) newMode = 2;
        else if (mode == 5) newMode = 3;
        else if (mode == 7) newMode = 6;
        if (newMode != mode) {
            pTok[ins - pTok] = (t & ~(UINT)(SM4_INTERP_MASK << SM4_INTERP_SHIFT)) |
                               (newMode << SM4_INTERP_SHIFT);
            patched++;
        }
    }
    if (patched)
        TR_LOG("dxbc: rewrote %u noperspective PS input decl(s) to linear",
               patched);
}

static int writeSHDRBlob(DxbcWriter *w, UINT fourcc,
                         const UINT *pTokens, size_t cbTokens)
{
    DXBCBlobHeader *pHdr = (DXBCBlobHeader *)writerAppend(w, NULL, sizeof(DXBCBlobHeader));
    if (!pHdr) return 0;
    pHdr->u32BlobType = fourcc;
    pHdr->cbBlob = 0;

    uint8_t *pStart = w->pPtr;
    if (!writerAppend(w, pTokens, cbTokens)) return 0;

    /* Patch in the writer's copy (never the caller's tokens); the
     * container hash is computed at finalize, after this. */
    if (npt_host_workaround_flags() & NPT_WA_LINEARIZE_NOPERSPECTIVE_PS_INPUT)
        patchNoPerspectiveDecls((UINT *)pStart, cbTokens);

    size_t pad = (4 - (cbTokens & 3)) & 3;
    if (pad && !writerAppend(w, NULL, pad)) return 0;

    pHdr->cbBlob = (uint32_t)(w->pPtr - pStart);
    return 1;
}

/* ---------- tritonBuildDxbc ----------
 *
 * Assembles the container, computes the hash, returns a HeapAlloc'd
 * buffer. The caller stores the result in TRITON_SHADER::pBytecode and
 * frees via HeapFree on destroy. */

void *tritonBuildDxbc(const UINT *pTokens, SIZE_T cbTokens,
                      const void *pInEntries,    UINT cInSigs,
                      const void *pOutEntries,   UINT cOutSigs,
                      const void *pPatchEntries, UINT cPatchSigs,
                      UINT entryStride,
                      SIZE_T *pcbOut)
{
    if (pcbOut) *pcbOut = 0;

    if (!pTokens || cbTokens < 8 || !pcbOut) {
        TR_LOG("tritonBuildDxbc: invalid args pTokens=%p cbTokens=%zu pcbOut=%p",
               (const void *)pTokens, (size_t)cbTokens, (const void *)pcbOut);
        return NULL;
    }
    if (entryStride != 12 && entryStride != 20 &&
        entryStride != sizeof(TRITON_DXBC_SIGNATURE)) {
        TR_LOG("tritonBuildDxbc: bad entryStride=%u", entryStride);
        return NULL;
    }

    const UINT progType = programType(pTokens);

    /* Scratch for the scan and the three merged entry sets; heap-allocated
     * to keep ~25 KiB off the caller's stack. */
    typedef struct TritonSigScratch {
        TritonShdrScan scan;
        TritonSigEntry aIn[TRITON_DXBC_MAX_SIG_ENTRIES];
        TritonSigEntry aOut[TRITON_DXBC_MAX_SIG_ENTRIES];
        TritonSigEntry aPatch[TRITON_DXBC_MAX_SIG_ENTRIES];
    } TritonSigScratch;
    TritonSigScratch *sc = (TritonSigScratch *)HeapAlloc(
        GetProcessHeap(), 0, sizeof(TritonSigScratch));
    if (!sc) {
        TR_LOG("tritonBuildDxbc: scratch HeapAlloc failed");
        return NULL;
    }

    tritonScanShdrDecls(pTokens, cbTokens, &sc->scan);

    /* Blob plan: ISGN, OSGN, [PCSG for HS/DS], SHDR. */
    const int fHullDomain = (progType == PROG_HS || progType == PROG_DS);
    const UINT cBlob = 3u + (fHullDomain ? 1u : 0u);

    UINT cIn  = tritonCollectSigEntries(pInEntries, cInSigs, entryStride,
                                        sc->scan.aIn, sc->scan.cIn, 1u, sc->aIn);
    UINT cOut = tritonCollectSigEntries(pOutEntries, cOutSigs, entryStride,
                                        sc->scan.aOut, sc->scan.cOut,
                                        progType == PROG_GS ? sc->scan.cStream : 1u,
                                        sc->aOut);
    UINT cPatch = 0;
    if (fHullDomain)
        cPatch = tritonCollectSigEntries(pPatchEntries, cPatchSigs, entryStride,
                                         sc->scan.aPatch, sc->scan.cPatch, 1u,
                                         sc->aPatch);

    cIn  = tritonAssignSemantics(sc->aIn,  cIn,  DXBC_BLOB_TYPE_ISGN, progType,
                                 &sc->scan);
    cOut = tritonAssignSemantics(sc->aOut, cOut, DXBC_BLOB_TYPE_OSGN, progType,
                                 &sc->scan);
    if (fHullDomain)
        cPatch = tritonAssignSemantics(sc->aPatch, cPatch, DXBC_BLOB_TYPE_PCSG,
                                       progType, &sc->scan);

    /* Pick the chunk variants fxc would emit; host translators are built
     * and tested against those forms. */
    const UINT verMajor = (pTokens[0] >> 4) & 0xFu;
    int fMinPrec = 0;
    for (UINT i = 0; i < cIn && !fMinPrec; ++i)
        fMinPrec = sc->aIn[i].minPrec != 0;
    for (UINT i = 0; i < cOut && !fMinPrec; ++i)
        fMinPrec = sc->aOut[i].minPrec != 0;
    for (UINT i = 0; i < cPatch && !fMinPrec; ++i)
        fMinPrec = sc->aPatch[i].minPrec != 0;
    const UINT fccIn    = fMinPrec ? DXBC_BLOB_TYPE_ISG1 : DXBC_BLOB_TYPE_ISGN;
    const UINT fccOut   = fMinPrec ? DXBC_BLOB_TYPE_OSG1
                        : (progType == PROG_GS && verMajor >= 5)
                            ? DXBC_BLOB_TYPE_OSG5 : DXBC_BLOB_TYPE_OSGN;
    const UINT fccPatch = fMinPrec ? DXBC_BLOB_TYPE_PSG1 : DXBC_BLOB_TYPE_PCSG;
    const UINT fccShdr  = (verMajor >= 5) ? DXBC_BLOB_TYPE_SHEX
                                          : DXBC_BLOB_TYPE_SHDR;

    /* Pre-size from actual signature counts. Per-entry budget = 32 B
     * element (largest variant) + 32 B for name+null+slop (longest name
     * "SV_RenderTargetArrayIndex" = 25 B + null = 26 B). Per-blob
     * overhead = BlobHeader (8) + IOSGN header (8) + 4 B alignment. */
    const size_t cSigEntries  = (size_t)cIn + cOut + cPatch;
    const size_t cbSigEntries = cSigEntries * (32 + 32);
    const size_t cbSigBlobs   = (size_t)cBlob * (sizeof(DXBCBlobHeader) + sizeof(DXBCBlobIOSGN) + 4);
    const size_t cbShdrBlob   = sizeof(DXBCBlobHeader) + (size_t)cbTokens + 4;
    const size_t cbHdr        = sizeof(DXBCHeader) + (cBlob - 1) * sizeof(uint32_t);
    const size_t cbInit       = cbHdr + cbSigEntries + cbSigBlobs + cbShdrBlob;

    DxbcWriter w;
    if (!writerInit(&w, cbInit)) {
        TR_LOG("tritonBuildDxbc: HeapAlloc(%zu) failed", cbInit);
        HeapFree(GetProcessHeap(), 0, sc);
        return NULL;
    }

    /* Container header. cbTotal / hash patched at finalize. */
    DXBCHeader *pHdr = (DXBCHeader *)writerAppend(&w, NULL, cbHdr);
    if (!pHdr) goto fail;
    pHdr->u32DXBC    = DXBC_MAGIC;
    pHdr->u32Version = 1;
    pHdr->cBlob      = cBlob;

    UINT iBlob = 0;

    pHdr->aBlobOffset[iBlob++] = (uint32_t)writerSize(&w);
    if (!writeSigBlob(&w, fccIn, DXBC_BLOB_TYPE_ISGN, progType, sc->aIn, cIn))
        goto fail;

    pHdr->aBlobOffset[iBlob++] = (uint32_t)writerSize(&w);
    if (!writeSigBlob(&w, fccOut, DXBC_BLOB_TYPE_OSGN, progType, sc->aOut, cOut))
        goto fail;

    if (fHullDomain) {
        pHdr->aBlobOffset[iBlob++] = (uint32_t)writerSize(&w);
        if (!writeSigBlob(&w, fccPatch, DXBC_BLOB_TYPE_PCSG, progType,
                          sc->aPatch, cPatch))
            goto fail;
    }

    pHdr->aBlobOffset[iBlob++] = (uint32_t)writerSize(&w);
    if (!writeSHDRBlob(&w, fccShdr, pTokens, (size_t)cbTokens))
        goto fail;

    HeapFree(GetProcessHeap(), 0, sc);
    sc = NULL;

    const uint32_t cbTotal = (uint32_t)writerSize(&w);
    pHdr->cbTotal = cbTotal;

    /* Hash covers [u32Version .. EOF]. Magic + hash field at 0..0x13
     * are excluded. */
    _Static_assert(offsetof(DXBCHeader, u32Version) == 0x14, "DXBC hash range");
    dxbcHash(&pHdr->u32Version, cbTotal - 0x14u, pHdr->au8Hash);

    void *pOut = w.pBegin;
    *pcbOut = (SIZE_T)cbTotal;
    w.pBegin = NULL;  /* transfer ownership */
    writerFree(&w);
    return pOut;

fail:
    TR_LOG("tritonBuildDxbc: assembly failed");
    if (sc)
        HeapFree(GetProcessHeap(), 0, sc);
    writerFree(&w);
    return NULL;
}

/* ---------- tritonBuildInputSigDxbc ----------
 *
 * Builds a minimal DXBC container carrying ONLY an ISGN chunk that describes
 * exactly the supplied (name, semanticIndex, register) entries.  Used by
 * tritonResolveInputLayout: D3D11 CreateInputLayout validates the element
 * descs against an input signature, and dxvk rejects any signature input not
 * covered by an element -- so the signature handed to it must mirror the
 * layout's elements, not the (often larger) bound VS signature.  Every entry
 * is declared as a 4-component float32 with Used == Mask, which is all dxvk's
 * input-layout validator inspects.  HeapAlloc'd; caller frees via HeapFree. */
void *tritonBuildInputSigDxbc(const char *const *pNames,
                              const UINT *pSemanticIndices,
                              const UINT *pRegisters,
                              UINT cEntries, SIZE_T *pcbOut)
{
    if (pcbOut) *pcbOut = 0;
    if (!pcbOut || !pNames || !pSemanticIndices || !pRegisters
        || cEntries == 0 || cEntries > TRITON_DXBC_MAX_SIG_ENTRIES)
        return NULL;

    /* cBlob == 1: the DXBCHeader's aBlobOffset[1] already holds the one
     * offset, so cbHdr is just sizeof(DXBCHeader). */
    const size_t cbHdr  = sizeof(DXBCHeader);
    const size_t cbInit = cbHdr
        + sizeof(DXBCBlobHeader) + sizeof(DXBCBlobIOSGN)
        + (size_t)cEntries * (sizeof(DXBCBlobIOSGNElement) + 32) + 16;

    DxbcWriter w;
    if (!writerInit(&w, cbInit)) {
        TR_LOG("tritonBuildInputSigDxbc: HeapAlloc(%zu) failed", cbInit);
        return NULL;
    }

    DXBCHeader *pHdr = (DXBCHeader *)writerAppend(&w, NULL, cbHdr);
    if (!pHdr) goto fail;
    pHdr->u32DXBC        = DXBC_MAGIC;
    pHdr->u32Version     = 1;
    pHdr->cBlob          = 1;
    pHdr->aBlobOffset[0] = (uint32_t)writerSize(&w);

    DXBCBlobHeader *pBlob = (DXBCBlobHeader *)writerAppend(&w, NULL, sizeof(DXBCBlobHeader));
    if (!pBlob) goto fail;
    pBlob->u32BlobType = DXBC_BLOB_TYPE_ISGN;
    pBlob->cbBlob      = 0; /* patched below */

    DXBCBlobIOSGN *pIO = (DXBCBlobIOSGN *)writerAppend(&w, NULL, sizeof(DXBCBlobIOSGN));
    if (!pIO) goto fail;
    pIO->cElement   = cEntries;
    pIO->offElement = 8;

    DXBCBlobIOSGNElement *pElems = (DXBCBlobIOSGNElement *)writerAppend(
        &w, NULL, (size_t)cEntries * sizeof(DXBCBlobIOSGNElement));
    if (!pElems) goto fail;

    for (UINT i = 0; i < cEntries; ++i) {
        pElems[i].offElementName   = 0; /* filled when the name is emitted */
        pElems[i].idxSemantic      = pSemanticIndices[i];
        pElems[i].enmSystemValue   = 0;  /* not a system-value input */
        pElems[i].enmComponentType = 3;  /* float32; dxvk ignores this for IL */
        pElems[i].idxRegister      = pRegisters[i];
        pElems[i].maskAndUsed      = 0x0Fu | (0x0Fu << 8); /* Mask=Used=xyzw */
    }

    /* Emit ASCIIZ names (offsets relative to the IOSGN blob), deduped. */
    for (UINT i = 0; i < cEntries; ++i) {
        if (pElems[i].offElementName != 0) continue;
        const char *nm = pNames[i] ? pNames[i] : "ATTRIB";
        UINT off = (UINT)(writerSize(&w) - ((const uint8_t *)pIO - w.pBegin));
        size_t cb = strlen(nm) + 1;
        if (!writerAppend(&w, nm, cb)) goto fail;
        pElems[i].offElementName = off;
        for (UINT j = i + 1; j < cEntries; ++j) {
            const char *nj = pNames[j] ? pNames[j] : "ATTRIB";
            if (pElems[j].offElementName == 0 && strcmp(nj, nm) == 0)
                pElems[j].offElementName = off;
        }
    }

    /* Pad the blob body to a 4-byte boundary. */
    size_t cbBody = writerSize(&w) - ((const uint8_t *)pIO - w.pBegin);
    size_t pad = (4 - (cbBody & 3)) & 3;
    if (pad && !writerAppend(&w, NULL, pad)) goto fail;
    cbBody += pad;
    pBlob->cbBlob = (uint32_t)cbBody;

    const uint32_t cbTotal = (uint32_t)writerSize(&w);
    pHdr->cbTotal = cbTotal;
    dxbcHash(&pHdr->u32Version, cbTotal - 0x14u, pHdr->au8Hash);

    void *pOut = w.pBegin;
    *pcbOut = (SIZE_T)cbTotal;
    w.pBegin = NULL; /* transfer ownership to caller */
    writerFree(&w);
    return pOut;

fail:
    TR_LOG("tritonBuildInputSigDxbc: assembly failed");
    writerFree(&w);
    return NULL;
}

/* ---------- tritonReconcileVsInputSig ----------
 *
 * D3DMetal derives its shader-side vertex fetch from the vertex shader's input
 * signature (ISGN), not from the input layout, and Triton types a DDI ATTRIB
 * input float32 -- the DDI does not carry the shader's scalar type
 * (RegisterComponentType is UNKNOWN even on the 11.1 signature entry for vertex
 * inputs). Fed an integer vertex format (e.g. R16G16_SINT) the fetch converts
 * to float and delivers garbage.
 *
 * At input-layout resolve the formats ARE known, so reconcile a copy of the
 * built VS DXBC to them: for each input register r in [0,32), compReg[r] (1=
 * uint, 2=sint, 3=float, 0=leave) overrides the input-signature component
 * type. Returns a heap copy (caller HeapFree's) with a recomputed container
 * hash, or NULL when nothing changed (caller keeps the original bytecode).
 * (The scalar-mask crash workaround is a separate, format-independent widen
 * applied earlier in writeSigBlob.) */
void *tritonReconcileVsInputSig(const void *pDxbc, SIZE_T cbDxbc,
                                const unsigned char compReg[32],
                                SIZE_T *pcbOut)
{
    if (pcbOut) *pcbOut = 0;
    if (!pDxbc || cbDxbc < sizeof(DXBCHeader) || !compReg)
        return NULL;

    const DXBCHeader *pSrc = (const DXBCHeader *)pDxbc;
    if (pSrc->u32DXBC != DXBC_MAGIC || pSrc->cbTotal > cbDxbc)
        return NULL;

    /* Locate the input-signature blob (ISGN, or ISG1 for a min-precision
     * shader). */
    const uint32_t cBlob = pSrc->cBlob;
    if ((size_t)offsetof(DXBCHeader, aBlobOffset)
            + (size_t)cBlob * sizeof(uint32_t) > pSrc->cbTotal)
        return NULL;
    uint32_t offIsgn = 0;
    UINT cbEntry = 0;
    int fStream = 0, fMinPrec = 0;
    for (uint32_t i = 0; i < cBlob; ++i) {
        uint32_t off = pSrc->aBlobOffset[i];
        if ((size_t)off + sizeof(DXBCBlobHeader) > pSrc->cbTotal)
            continue;
        const DXBCBlobHeader *bh =
            (const DXBCBlobHeader *)((const uint8_t *)pDxbc + off);
        if (bh->u32BlobType == DXBC_BLOB_TYPE_ISGN ||
            bh->u32BlobType == DXBC_BLOB_TYPE_ISG1) {
            offIsgn = off;
            tritonSigBlobLayout(bh->u32BlobType, &cbEntry, &fStream, &fMinPrec);
            break;
        }
    }
    if (!offIsgn)
        return NULL;
    const UINT offBase = fStream ? 4u : 0u;

    const size_t offElems =
        (size_t)offIsgn + sizeof(DXBCBlobHeader) + sizeof(DXBCBlobIOSGN);
    /* The blob scan validated only the 8-byte DXBCBlobHeader; the DXBCBlobIOSGN
     * that follows must also be fully in bounds before cElement is read. All
     * bounds arithmetic here is size_t, so blob-derived offsets cannot wrap
     * 32-bit. */
    if (offElems > pSrc->cbTotal)
        return NULL;
    const DXBCBlobIOSGN *pIoSrc =
        (const DXBCBlobIOSGN *)((const uint8_t *)pDxbc + offIsgn + sizeof(DXBCBlobHeader));
    if (offElems + (size_t)pIoSrc->cElement * cbEntry > pSrc->cbTotal)
        return NULL;

    /* Copy first; the app's bytecode is never mutated in place. */
    uint8_t *pCopy = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, pSrc->cbTotal);
    if (!pCopy) return NULL;
    memcpy(pCopy, pDxbc, pSrc->cbTotal);

    int changed = 0;
    for (uint32_t i = 0; i < pIoSrc->cElement; ++i) {
        uint8_t *e = pCopy + offElems + (size_t)i * cbEntry;
        UINT *pCompType = (UINT *)(e + offBase + 12);
        const UINT reg  = *(const UINT *)(e + offBase + 16);
        if (reg >= 32) continue;
        unsigned char want = compReg[reg];
        if (want != 0 && *pCompType != want) {
            *pCompType = want;
            changed = 1;
        }
    }

    if (!changed) {
        HeapFree(GetProcessHeap(), 0, pCopy);
        return NULL;
    }

    DXBCHeader *pHdr = (DXBCHeader *)pCopy;
    dxbcHash(&pHdr->u32Version, pHdr->cbTotal - 0x14u, pHdr->au8Hash);
    if (pcbOut) *pcbOut = (SIZE_T)pHdr->cbTotal;
    return pCopy;
}

/* If any of these fire, the container format is misaligned and the
 * runtime / dxvk will reject every shader. */
_Static_assert(sizeof(DXBCBlobHeader)        ==  8, "DXBCBlobHeader");
_Static_assert(sizeof(DXBCBlobIOSGNElement)  == 24, "DXBCBlobIOSGNElement");
_Static_assert(sizeof(DXBCBlobIOSGN)         ==  8, "DXBCBlobIOSGN (header only)");
_Static_assert(offsetof(DXBCHeader, au8Hash)     ==  4, "DXBCHeader.au8Hash");
_Static_assert(offsetof(DXBCHeader, u32Version)  == 20, "DXBCHeader.u32Version");
_Static_assert(offsetof(DXBCHeader, cbTotal)     == 24, "DXBCHeader.cbTotal");
_Static_assert(offsetof(DXBCHeader, cBlob)       == 28, "DXBCHeader.cBlob");
_Static_assert(offsetof(DXBCHeader, aBlobOffset) == 32, "DXBCHeader.aBlobOffset");
