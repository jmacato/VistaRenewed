/* Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D9 fixed-function shader generation. Constants contain all numeric state;
 * instruction variants depend only on operations, declarations and light kinds.
 */
#include "triton9_fixed.h"
#include "triton9_shader_token_contract.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace Triton9Fixed {
namespace {
constexpr unsigned X = 1, Y = 2, Z = 4, W = 8, XYZ = 7, ALL = 15;
unsigned type(unsigned t) { return ((t << 28) & D3DSP_REGTYPE_MASK) |
                                   ((t << 8) & D3DSP_REGTYPE_MASK2); }
unsigned dst(unsigned t, unsigned n, unsigned mask = ALL, bool sat = false) {
    return 0x80000000u | type(t) | n | (mask << 16) |
           (sat ? D3DSPDM_SATURATE : 0);
}
unsigned src(unsigned t, unsigned n, unsigned sw = 0xe4) {
    return 0x80000000u | type(t) | n | (sw << 16);
}
unsigned rep(unsigned c) { return c * 0x55; }
unsigned r(unsigned n, unsigned sw = 0xe4) { return src(D3DSPR_TEMP, n, sw); }
unsigned c(unsigned n, unsigned sw = 0xe4) { return src(D3DSPR_CONST, n, sw); }
unsigned neg(unsigned value) { return value | D3DSPSM_NEG; }
struct Emit {
    std::vector<unsigned> &v;
    void op(unsigned code, unsigned d, std::initializer_list<unsigned> sources) {
        v.push_back(code | ((sources.size() + 1) << D3DSI_INSTLENGTH_SHIFT));
        v.push_back(d); v.insert(v.end(), sources.begin(), sources.end());
    }
    void mov(unsigned d, unsigned s) { op(D3DSIO_MOV, d, {s}); }
    void dcl(unsigned usage, unsigned index, unsigned t, unsigned n) {
        v.insert(v.end(), {D3DSIO_DCL | (2 << D3DSI_INSTLENGTH_SHIFT),
            triton9_sm2_dcl_semantic(usage, index), dst(t, n)});
    }
    void norm(unsigned target, unsigned value) {
        op(D3DSIO_DP3, dst(D3DSPR_TEMP, 30, X), {value, value});
        // Clamp zero length before rsq. A zero vector remains zero.
        op(D3DSIO_MAX, dst(D3DSPR_TEMP, 30, X), {r(30, 0), c(2, rep(3))});
        op(D3DSIO_RSQ, dst(D3DSPR_TEMP, 30, X), {r(30, 0)});
        op(D3DSIO_MUL, dst(D3DSPR_TEMP, target, XYZ), {value, r(30, 0)});
    }
};
float bits(unsigned value) { float f; std::memcpy(&f, &value, 4); return f; }
Constant color(unsigned value) {
    return {float((value >> 16) & 255) / 255, float((value >> 8) & 255) / 255,
            float(value & 255) / 255, float(value >> 24) / 255};
}
Constant color(const D3DCOLORVALUE &v) { return {v.r, v.g, v.b, v.a}; }
const Input *input(const State &s, unsigned usage, unsigned index = 0) {
    for (const auto &i : s.inputs) if (i.usage == usage && i.index == index) return &i;
    return nullptr;
}
unsigned in(const State &s, unsigned usage, unsigned index = 0,
            unsigned fallback = 0, unsigned sw = 0xe4) {
    const auto *i = input(s, usage, index);
    return i ? src(D3DSPR_INPUT, i->reg, sw) : c(0, rep(fallback));
}
void transpose(const D3DMATRIX &m, std::vector<Constant> &v, unsigned base) {
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j) v[base + i][j] = m.m[j][i];
}
Constant transform(const D3DVECTOR &v, const D3DMATRIX &m, float w) {
    Constant out{};
    for (unsigned j = 0; j < 4; ++j)
        out[j] = v.x * m.m[0][j] + v.y * m.m[1][j] + v.z * m.m[2][j] + w * m.m[3][j];
    return out;
}
void normalize(Constant &v) {
    float len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len > 0) for (unsigned j = 0; j < 3; ++j) v[j] /= len;
}
void vertexConstants(const State &s, Program &p) {
    auto &k = p.constants;
    unsigned blend = s.render[D3DRS_VERTEXBLEND];
    unsigned matrices = s.render[D3DRS_INDEXEDVERTEXBLENDENABLE] &&
        blend != D3DVBF_DISABLE ? 256 : blend <= D3DVBF_3WEIGHTS ? blend + 1 : 1;
    k.assign(MatrixBase + matrices * MatrixStride, Constant{});
    k[0] = {0, 1, 2, .5f};
    k[1] = {bits(s.render[D3DRS_FOGEND]), bits(s.render[D3DRS_FOGDENSITY]), 0, -1.4426950408889634f};
    float distance = bits(s.render[D3DRS_FOGEND]) - bits(s.render[D3DRS_FOGSTART]);
    k[1][2] = distance ? 1 / distance : 0;
    k[2] = {float(MatrixStride) * (s.normalizedBlendIndices ? 255 : 1), bits(s.render[D3DRS_TWEENFACTOR]), s.material.Power, 1e-30f};
    k[3] = {bits(s.render[D3DRS_POINTSIZE]), bits(s.render[D3DRS_POINTSIZE_MIN]),
        bits(s.render[D3DRS_POINTSIZE_MAX]), s.viewportHeight};
    if (k[3][2] <= 0) k[3][2] = 64;
    transpose(s.projection, k, 4);
    k[8] = color(s.material.Diffuse); k[9] = color(s.material.Ambient);
    k[10] = color(s.material.Specular); k[11] = color(s.material.Emissive);
    k[12] = color(s.render[D3DRS_AMBIENT]);
    k[13] = {bits(s.render[D3DRS_POINTSCALE_A]), bits(s.render[D3DRS_POINTSCALE_B]),
             bits(s.render[D3DRS_POINTSCALE_C]), 0};
    for (unsigned i = 0; i < s.lights.size(); ++i) {
        const auto &l = s.lights[i]; unsigned base = 16 + i * 7;
        k[base] = color(l.Diffuse); k[base+1] = color(l.Ambient);
        k[base+2] = color(l.Specular);
        k[base+3] = transform(l.Position, s.view, 1); k[base+3][3] = l.Range;
        k[base+4] = transform(l.Direction, s.view, 0); normalize(k[base+4]);
        k[base+5] = {l.Attenuation0, l.Attenuation1, l.Attenuation2, l.Falloff};
        float inner = std::cos(l.Theta * .5f), outer = std::cos(l.Phi * .5f);
        k[base+6] = {inner, outer, inner != outer ? 1 / (inner - outer) : 0, 0};
    }
    for (unsigned i = 0; i < StageCount; ++i) transpose(s.texture[i], k, 72 + i*4);
    for (unsigned i = 0; i < matrices; ++i) {
        D3DMATRIX m, inv;
        multiply(s.world[i], s.view, m); transpose(m, k, MatrixBase + i*MatrixStride);
        // Normals are row vectors multiplied by inverse transpose.
        if (!inverse(m, inv)) inv = {};
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned col = 0; col < 3; ++col)
                k[MatrixBase + i*MatrixStride + 4 + row][col] = inv.m[row][col];
    }
}
}

bool equal(const State &a, const State &b) {
    return !std::memcmp(a.render, b.render, sizeof(a.render)) &&
        !std::memcmp(a.stages, b.stages, sizeof(a.stages)) &&
        !std::memcmp(a.world, b.world, sizeof(a.world)) &&
        !std::memcmp(&a.view, &b.view, sizeof(a.view)) &&
        !std::memcmp(&a.projection, &b.projection, sizeof(a.projection)) &&
        !std::memcmp(a.texture, b.texture, sizeof(a.texture)) &&
        !std::memcmp(&a.material, &b.material, sizeof(a.material)) &&
        a.lights.size() == b.lights.size() &&
        (a.lights.empty() || !std::memcmp(a.lights.data(), b.lights.data(), a.lights.size()*sizeof(D3DLIGHT9))) &&
        a.inputs.size() == b.inputs.size() &&
        (a.inputs.empty() || !std::memcmp(a.inputs.data(), b.inputs.data(), a.inputs.size()*sizeof(Input))) &&
        a.diffuse == b.diffuse && a.specular == b.specular &&
        a.transformed == b.transformed && a.texcoords == b.texcoords &&
        a.normalizedBlendIndices == b.normalizedBlendIndices &&
        a.viewportHeight == b.viewportHeight;
}

void multiply(const D3DMATRIX &a, const D3DMATRIX &b, D3DMATRIX &out) {
    D3DMATRIX result{};
    for (unsigned i=0; i<4; ++i) for (unsigned j=0; j<4; ++j)
        for (unsigned k=0; k<4; ++k) result.m[i][j] += a.m[i][k]*b.m[k][j];
    out = result;
}
bool inverse(const D3DMATRIX &matrix, D3DMATRIX &out) {
    double a[4][8]{};
    for (unsigned i=0; i<4; ++i) for (unsigned j=0; j<4; ++j) {
        a[i][j] = matrix.m[i][j]; a[i][j+4] = i == j;
    }
    for (unsigned i=0; i<4; ++i) {
        unsigned pivot=i;
        for (unsigned j=i+1; j<4; ++j) if (std::abs(a[j][i]) > std::abs(a[pivot][i])) pivot=j;
        if (a[pivot][i] == 0 || !std::isfinite(a[pivot][i])) return false;
        for (unsigned j=0; j<8; ++j) std::swap(a[i][j], a[pivot][j]);
        double scale=a[i][i]; for (double &v : a[i]) v /= scale;
        for (unsigned j=0; j<4; ++j) if (j != i) {
            double f=a[j][i]; for (unsigned k=0; k<8; ++k) a[j][k] -= f*a[i][k];
        }
    }
    for (unsigned i=0; i<4; ++i) for (unsigned j=0; j<4; ++j) out.m[i][j]=float(a[i][j+4]);
    return true;
}

HRESULT vertex(const State &s, Program &p) {
    p.fixedFunction = true;
    if (!input(s, D3DDECLUSAGE_POSITION) || s.lights.size() > 8) return E_INVALIDARG;
    try {
        vertexConstants(s, p);
        p.tokens.clear(); p.tokens.push_back(D3DVS_VERSION(2, 0)); Emit e{p.tokens};
        for (const auto &i : s.inputs) e.dcl(i.usage, i.index, D3DSPR_INPUT, i.reg);
        // r0=view position, r1=view normal; r4/r5=object position/normal.
        e.mov(dst(D3DSPR_TEMP,4), in(s,D3DDECLUSAGE_POSITION));
        e.mov(dst(D3DSPR_TEMP,5), in(s,D3DDECLUSAGE_NORMAL));
        unsigned blend = s.render[D3DRS_VERTEXBLEND];
        if (blend == D3DVBF_TWEENING) {
            if (!input(s,D3DDECLUSAGE_POSITION,1)) return E_INVALIDARG;
            e.op(D3DSIO_LRP,dst(D3DSPR_TEMP,4),{c(2,rep(1)),in(s,D3DDECLUSAGE_POSITION,1),r(4)});
            if (input(s,D3DDECLUSAGE_NORMAL,1))
                e.op(D3DSIO_LRP,dst(D3DSPR_TEMP,5),{c(2,rep(1)),in(s,D3DDECLUSAGE_NORMAL,1),r(5)});
        }
        bool indexed = s.render[D3DRS_INDEXEDVERTEXBLENDENABLE] != 0 && blend != D3DVBF_DISABLE;
        unsigned weights = blend <= D3DVBF_3WEIGHTS ? blend : 0;
        if (weights && !input(s,D3DDECLUSAGE_BLENDWEIGHT)) return E_INVALIDARG;
        if (indexed && !input(s,D3DDECLUSAGE_BLENDINDICES)) return E_INVALIDARG;
        e.mov(dst(D3DSPR_TEMP,0),c(0,0)); e.mov(dst(D3DSPR_TEMP,1),c(0,0));
        e.mov(dst(D3DSPR_TEMP,3,W),c(0,rep(1)));
        if (indexed) e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,29),{in(s,D3DDECLUSAGE_BLENDINDICES),c(2,0)});
        for (unsigned i=0; i<=weights; ++i) {
            unsigned base=MatrixBase + (indexed ? 0 : i*MatrixStride);
            if (indexed) e.op(D3DSIO_MOVA,dst(D3DSPR_ADDR,0,X),{r(29,rep(i))});
            auto matrix = [&](unsigned code, unsigned target, unsigned value, unsigned reg, unsigned mask) {
                if (indexed) e.op(code,dst(D3DSPR_TEMP,target,mask),{value,c(reg)|D3DSHADER_ADDRESSMODE_MASK,src(D3DSPR_ADDR,0,0)});
                else e.op(code,dst(D3DSPR_TEMP,target,mask),{value,c(reg)});
            };
            matrix(D3DSIO_M4x4,2,r(4),base,ALL);
            matrix(D3DSIO_M3x3,28,r(5),base+4,XYZ);
            unsigned weight = i<weights ? in(s,D3DDECLUSAGE_BLENDWEIGHT,0,0,rep(i)) : r(3,rep(3));
            e.op(D3DSIO_MAD,dst(D3DSPR_TEMP,0),{r(2),weight,r(0)});
            e.op(D3DSIO_MAD,dst(D3DSPR_TEMP,1,XYZ),{r(28),weight,r(1)});
            if (i<weights) e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,3,W),{r(3,rep(3)),neg(weight)});
        }
        if (s.render[D3DRS_NORMALIZENORMALS]) e.norm(1,r(1));
        e.op(D3DSIO_M4x4,dst(D3DSPR_RASTOUT,D3DSRO_POSITION),{r(0),c(4)});
        if (s.render[D3DRS_LIGHTING]) {
            unsigned sources[]={D3DRS_DIFFUSEMATERIALSOURCE,D3DRS_AMBIENTMATERIALSOURCE,
                D3DRS_SPECULARMATERIALSOURCE,D3DRS_EMISSIVEMATERIALSOURCE};
            for(unsigned i=0;i<4;++i) {
                unsigned source = s.render[D3DRS_COLORVERTEX] ? s.render[sources[i]] : D3DMCS_MATERIAL;
                const Input *attribute = source == D3DMCS_COLOR1 ? input(s,D3DDECLUSAGE_COLOR,0) :
                    source == D3DMCS_COLOR2 ? input(s,D3DDECLUSAGE_COLOR,1) : nullptr;
                e.mov(dst(D3DSPR_TEMP,6+i),attribute ? src(D3DSPR_INPUT,attribute->reg) : c(8+i));
            }
            e.op(D3DSIO_MAD,dst(D3DSPR_TEMP,10),{r(7),c(12),r(9)});
            e.mov(dst(D3DSPR_TEMP,11),c(0,0));
            // View direction is toward the eye; an infinite viewer is at -Z.
            if (s.render[D3DRS_LOCALVIEWER]) e.norm(12,neg(r(0)));
            else { e.mov(dst(D3DSPR_TEMP,12),c(0,0)); e.mov(dst(D3DSPR_TEMP,12,Z),neg(c(0,rep(1)))); }
            for(unsigned i=0;i<s.lights.size();++i) {
                unsigned b=16+i*7;
                if(s.lights[i].Type == D3DLIGHT_DIRECTIONAL) {
                    e.mov(dst(D3DSPR_TEMP,13,XYZ),neg(c(b+4)));
                    e.mov(dst(D3DSPR_TEMP,14,X),c(0,rep(1)));
                } else {
                    e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,13,XYZ),{c(b+3),neg(r(0))});
                    e.op(D3DSIO_DP3,dst(D3DSPR_TEMP,14,Y),{r(13),r(13)});
                    e.op(D3DSIO_MAX,dst(D3DSPR_TEMP,14,Y),{r(14,rep(1)),c(2,rep(3))});
                    e.op(D3DSIO_RSQ,dst(D3DSPR_TEMP,14,Z),{r(14,rep(1))});
                    e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,13,XYZ),{r(13),r(14,rep(2))});
                    e.op(D3DSIO_RCP,dst(D3DSPR_TEMP,14,Z),{r(14,rep(2))});
                    e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,14,X),{r(14,rep(1)),c(b+5,rep(2))});
                    e.op(D3DSIO_MAD,dst(D3DSPR_TEMP,14,X),{r(14,rep(2)),c(b+5,rep(1)),r(14,0)});
                    e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,14,X),{r(14,0),c(b+5,0)});
                    e.op(D3DSIO_RCP,dst(D3DSPR_TEMP,14,X),{r(14,0)});
                    e.op(D3DSIO_SGE,dst(D3DSPR_TEMP,14,Y),{c(b+3,rep(3)),r(14,rep(2))});
                    e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,14,X),{r(14,0),r(14,rep(1))});
                    if(s.lights[i].Type == D3DLIGHT_SPOT) {
                        e.op(D3DSIO_DP3,dst(D3DSPR_TEMP,15,X),{neg(r(13)),c(b+4)});
                        if (s.lights[i].Theta == s.lights[i].Phi)
                            e.op(D3DSIO_SGE,dst(D3DSPR_TEMP,15,X),{r(15,0),c(b+6,rep(1))});
                        else {
                            // Falloff zero still produces no light outside the cone.
                            e.op(D3DSIO_SLT,dst(D3DSPR_TEMP,15,Y),{c(b+6,rep(1)),r(15,0)});
                            e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,15,X),{r(15,0),neg(c(b+6,rep(1)))});
                            e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,15,X,true),{r(15,0),c(b+6,rep(2))});
                            e.op(D3DSIO_POW,dst(D3DSPR_TEMP,15,X),{r(15,0),c(b+5,rep(3))});
                            e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,15,X),{r(15,0),r(15,rep(1))});
                        }
                        e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,14,X),{r(14,0),r(15,0)});
                    }
                }
                e.op(D3DSIO_DP3,dst(D3DSPR_TEMP,15,X),{r(1),r(13)});
                e.op(D3DSIO_MAX,dst(D3DSPR_TEMP,15,X),{r(15,0),c(0,0)});
                e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,16),{r(6),c(b)});
                e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,16),{r(16),r(15,0)});
                e.op(D3DSIO_MAD,dst(D3DSPR_TEMP,16),{r(7),c(b+1),r(16)});
                e.op(D3DSIO_MAD,dst(D3DSPR_TEMP,10,XYZ),{r(16),r(14,0),r(10)});
                if(s.render[D3DRS_SPECULARENABLE]) {
                    e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,17,XYZ),{r(13),r(12)}); e.norm(17,r(17));
                    e.op(D3DSIO_DP3,dst(D3DSPR_TEMP,17,X),{r(1),r(17)});
                    e.op(D3DSIO_MAX,dst(D3DSPR_TEMP,17,X),{r(17,0),c(0,0)});
                    e.op(D3DSIO_POW,dst(D3DSPR_TEMP,17,X),{r(17,0),c(2,rep(2))});
                    e.op(D3DSIO_SLT,dst(D3DSPR_TEMP,15,X),{c(0,0),r(15,0)});
                    e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,17,X),{r(17,0),r(15,0)});
                    e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,17,X),{r(17,0),r(14,0)});
                    e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,16),{r(8),c(b+2)});
                    e.op(D3DSIO_MAD,dst(D3DSPR_TEMP,11,XYZ),{r(16),r(17,0),r(11)});
                }
            }
            e.mov(dst(D3DSPR_TEMP,10,W),r(6,rep(3)));
            e.mov(dst(D3DSPR_TEMP,11,W),c(0,rep(1)));
            e.mov(dst(D3DSPR_ATTROUT,0,ALL,true),r(10));
            e.mov(dst(D3DSPR_ATTROUT,1,ALL,true),r(11));
        } else {
            e.mov(dst(D3DSPR_ATTROUT,0),in(s,D3DDECLUSAGE_COLOR,0,1));
            e.mov(dst(D3DSPR_ATTROUT,1),in(s,D3DDECLUSAGE_COLOR,1));
        }
        // Vertex fog NONE consumes input specular alpha, or defaults to one.
        unsigned fog=s.render[D3DRS_FOGENABLE] ? s.render[D3DRS_FOGVERTEXMODE] : D3DFOG_NONE;
        unsigned fogValue=in(s,D3DDECLUSAGE_COLOR,1,1,rep(3));
        if(fog != D3DFOG_NONE) {
            if(s.render[D3DRS_RANGEFOGENABLE]) {
                e.op(D3DSIO_DP3,dst(D3DSPR_TEMP,18,X),{r(0),r(0)});
                e.op(D3DSIO_RSQ,dst(D3DSPR_TEMP,18,X),{r(18,0)});
                e.op(D3DSIO_RCP,dst(D3DSPR_TEMP,18,X),{r(18,0)});
            } else e.op(D3DSIO_ABS,dst(D3DSPR_TEMP,18,X),{r(0,rep(2))});
            if(fog==D3DFOG_LINEAR) {
                e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,18,X),{c(1,0),neg(r(18,0))});
                e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,18,X,true),{r(18,0),c(1,rep(2))});
            } else {
                e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,18,X),{r(18,0),c(1,rep(1))});
                if(fog==D3DFOG_EXP2) e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,18,X),{r(18,0),r(18,0)});
                e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,18,X),{r(18,0),c(1,rep(3))});
                e.op(D3DSIO_EXP,dst(D3DSPR_TEMP,18,X),{r(18,0)});
            }
            fogValue=r(18,0);
        }
        e.mov(dst(D3DSPR_RASTOUT,D3DSRO_FOG,X,true),fogValue);
        unsigned size = input(s,D3DDECLUSAGE_PSIZE) ? in(s,D3DDECLUSAGE_PSIZE,0,0,0) : c(3,0);
        e.mov(dst(D3DSPR_TEMP,19,X),size);
        if(s.render[D3DRS_POINTSCALEENABLE]) {
            e.op(D3DSIO_DP3,dst(D3DSPR_TEMP,19,Y),{r(0),r(0)});
            e.op(D3DSIO_RSQ,dst(D3DSPR_TEMP,19,Z),{r(19,rep(1))});
            e.op(D3DSIO_RCP,dst(D3DSPR_TEMP,19,Z),{r(19,rep(2))});
            e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,19,W),{r(19,rep(1)),c(13,rep(2))});
            e.op(D3DSIO_MAD,dst(D3DSPR_TEMP,19,W),{r(19,rep(2)),c(13,rep(1)),r(19,rep(3))});
            e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,19,W),{r(19,rep(3)),c(13,0)});
            e.op(D3DSIO_RSQ,dst(D3DSPR_TEMP,19,W),{r(19,rep(3))});
            e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,19,X),{r(19,0),r(19,rep(3))});
            e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,19,X),{r(19,0),c(3,rep(3))});
        }
        e.op(D3DSIO_MAX,dst(D3DSPR_TEMP,19,X),{r(19,0),c(3,rep(1))});
        e.op(D3DSIO_MIN,dst(D3DSPR_RASTOUT,D3DSRO_POINT_SIZE,X),{r(19,0),c(3,rep(2))});
        for(unsigned stage=0;stage<StageCount;++stage) {
            const auto &t=s.stages[stage]; unsigned mode=t.texcoord & 0xffff0000u;
            unsigned components=4;
            e.mov(dst(D3DSPR_TEMP,20),c(0,0));
            if(mode==D3DTSS_TCI_PASSTHRU) {
                const auto *coord=input(s,D3DDECLUSAGE_TEXCOORD,t.texcoord & 0xffff);
                if(coord) { e.mov(dst(D3DSPR_TEMP,20),src(D3DSPR_INPUT,coord->reg)); components=coord->components; }
                else components=0;
            } else if(mode==D3DTSS_TCI_CAMERASPACENORMAL) { e.mov(dst(D3DSPR_TEMP,20,XYZ),r(1)); components=3; }
            else if(mode==D3DTSS_TCI_CAMERASPACEPOSITION) { e.mov(dst(D3DSPR_TEMP,20,XYZ),r(0)); components=3; }
            else if(mode==D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR || mode==D3DTSS_TCI_SPHEREMAP) {
                e.norm(20,r(0));
                e.op(D3DSIO_DP3,dst(D3DSPR_TEMP,21,X),{r(1),r(20)});
                e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,21,X),{r(21,0),c(0,rep(2))});
                e.op(D3DSIO_MAD,dst(D3DSPR_TEMP,20,XYZ),{neg(r(1)),r(21,0),r(20)});
                components=3;
                if(mode==D3DTSS_TCI_SPHEREMAP) {
                    e.mov(dst(D3DSPR_TEMP,21,XYZ),r(20));
                    e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,21,Z),{r(21,rep(2)),c(0,rep(1))});
                    e.op(D3DSIO_DP3,dst(D3DSPR_TEMP,21,X),{r(21),r(21)});
                    e.op(D3DSIO_RSQ,dst(D3DSPR_TEMP,21,X),{r(21,0)});
                    e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,21,X),{r(21,0),c(0,rep(3))});
                    e.op(D3DSIO_MAD,dst(D3DSPR_TEMP,20,X|Y),{r(20),r(21,0),c(0,rep(3))});
                    e.mov(dst(D3DSPR_TEMP,20,Z),c(0,0)); components=2;
                }
            } else return E_INVALIDARG;
            if(components<4) {
                e.mov(dst(D3DSPR_TEMP,20,ALL & ~((1u<<components)-1)),c(0,0));
                e.mov(dst(D3DSPR_TEMP,20,1u<<components),c(0,rep(1)));
            }
            unsigned count=t.transform & ~D3DTTFF_PROJECTED;
            if(count) {
                if(count>4) return E_INVALIDARG;
                e.op(D3DSIO_M4x4,dst(D3DSPR_TEMP,21),{r(20),c(72+stage*4)});
                e.mov(dst(D3DSPR_TEMP,20),r(21));
            }
            if(t.transform & D3DTTFF_PROJECTED) {
                if(count<2 || count>4) return E_INVALIDARG;
                e.mov(dst(D3DSPR_TEMP,20,W),r(20,rep(count-1)));
            }
            unsigned outputCount = count ? count : components;
            unsigned total = (t.transform & D3DTTFF_PROJECTED) ? 3 : 4;
            if (outputCount < total)
                e.mov(dst(D3DSPR_TEMP,20,((1u << total) - 1) & ~((1u << outputCount) - 1)),c(0,0));
            e.mov(dst(D3DSPR_TEXCRDOUT,stage),r(20));
        }
        p.tokens.push_back(D3DVS_END());
    } catch (...) { return E_OUTOFMEMORY; }
    return S_OK;
}

namespace {
bool usesArg(unsigned op, unsigned arg) {
    if(op==D3DTOP_DISABLE || op==D3DTOP_BUMPENVMAP || op==D3DTOP_BUMPENVMAPLUMINANCE) return false;
    if(arg==0) return op==D3DTOP_MULTIPLYADD || op==D3DTOP_LERP;
    if(op==D3DTOP_SELECTARG1 || op==D3DTOP_PREMODULATE) return arg==1;
    if(op==D3DTOP_SELECTARG2) return arg==2;
    return true;
}
bool textureUsed(const Stage &t, bool colorOnly=false) {
    if(t.colorOp==D3DTOP_BUMPENVMAP || t.colorOp==D3DTOP_BUMPENVMAPLUMINANCE ||
       t.colorOp==D3DTOP_BLENDTEXTUREALPHA || t.colorOp==D3DTOP_BLENDTEXTUREALPHAPM) return true;
    for(unsigned i=0;i<3;++i) {
        if(usesArg(t.colorOp,i) && (t.colorArg[i]&D3DTA_SELECTMASK)==D3DTA_TEXTURE) return true;
        if(!colorOnly && usesArg(t.alphaOp,i) && (t.alphaArg[i]&D3DTA_SELECTMASK)==D3DTA_TEXTURE) return true;
    }
    return !colorOnly && (t.alphaOp==D3DTOP_BLENDTEXTUREALPHA || t.alphaOp==D3DTOP_BLENDTEXTUREALPHAPM);
}
bool loadArg(Emit &e, const State &s, unsigned stage, unsigned argument, unsigned target) {
    unsigned value;
    switch(argument & D3DTA_SELECTMASK) {
    case D3DTA_DIFFUSE: value=s.diffuse ? src(D3DSPR_INPUT,0) : c(0,rep(1)); break;
    case D3DTA_CURRENT: value=r(7); break;
    case D3DTA_TEXTURE: value=r(2); break;
    case D3DTA_TFACTOR: value=c(1); break;
    case D3DTA_SPECULAR: value=s.specular ? src(D3DSPR_INPUT,1) : c(0,0); break;
    case D3DTA_TEMP: value=r(1); break;
    case D3DTA_CONSTANT: value=c(8+stage); break;
    default: return false;
    }
    e.mov(dst(D3DSPR_TEMP,target),value);
    if(argument & D3DTA_COMPLEMENT)
        e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,target),{c(0,rep(1)),neg(r(target))});
    if(argument & D3DTA_ALPHAREPLICATE)
        e.mov(dst(D3DSPR_TEMP,target),r(target,rep(3)));
    return !(argument & ~(D3DTA_SELECTMASK | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE));
}
bool operation(Emit &e, const State &s, unsigned op, unsigned mask) {
    unsigned d=dst(D3DSPR_TEMP,3,mask,true);
    unsigned a0=r(8),a1=r(9),a2=r(10);
    switch(op) {
    case D3DTOP_DISABLE: return true;
    case D3DTOP_SELECTARG1:
    case D3DTOP_PREMODULATE: e.mov(d,a1); break;
    case D3DTOP_SELECTARG2: e.mov(d,a2); break;
    case D3DTOP_MODULATE: e.op(D3DSIO_MUL,d,{a1,a2}); break;
    case D3DTOP_MODULATE2X:
    case D3DTOP_MODULATE4X:
        e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,11),{a1,a2});
        e.op(D3DSIO_MUL,d,{r(11),op==D3DTOP_MODULATE2X ? c(0,rep(2)) : c(2,0)}); break;
    case D3DTOP_ADD: e.op(D3DSIO_ADD,d,{a1,a2}); break;
    case D3DTOP_ADDSIGNED:
    case D3DTOP_ADDSIGNED2X:
        e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,11),{a1,a2});
        e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,11),{r(11),neg(c(0,rep(3)))});
        e.op(D3DSIO_MUL,d,{r(11),c(0,rep(op==D3DTOP_ADDSIGNED ? 1 : 2))}); break;
    case D3DTOP_SUBTRACT: e.op(D3DSIO_ADD,d,{a1,neg(a2)}); break;
    case D3DTOP_ADDSMOOTH:
        e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,11),{c(0,rep(1)),neg(a1)});
        e.op(D3DSIO_MAD,d,{r(11),a2,a1}); break;
    case D3DTOP_BLENDDIFFUSEALPHA:
    case D3DTOP_BLENDTEXTUREALPHA:
    case D3DTOP_BLENDFACTORALPHA:
    case D3DTOP_BLENDCURRENTALPHA: {
        unsigned alpha=op==D3DTOP_BLENDDIFFUSEALPHA ? (s.diffuse ? src(D3DSPR_INPUT,0,rep(3)) : c(0,rep(1))) :
            op==D3DTOP_BLENDTEXTUREALPHA ? r(2,rep(3)) : op==D3DTOP_BLENDFACTORALPHA ? c(1,rep(3)) : r(0,rep(3));
        e.op(D3DSIO_LRP,d,{alpha,a1,a2}); break;
    }
    case D3DTOP_BLENDTEXTUREALPHAPM:
        e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,11),{c(0,rep(1)),neg(r(2,rep(3)))});
        e.op(D3DSIO_MAD,d,{r(11),a2,a1}); break;
    case D3DTOP_MODULATEALPHA_ADDCOLOR: e.op(D3DSIO_MAD,d,{r(9,rep(3)),a2,a1}); break;
    case D3DTOP_MODULATECOLOR_ADDALPHA: e.op(D3DSIO_MAD,d,{a1,a2,r(9,rep(3))}); break;
    case D3DTOP_MODULATEINVALPHA_ADDCOLOR:
        e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,11),{c(0,rep(1)),neg(r(9,rep(3)))});
        e.op(D3DSIO_MAD,d,{r(11),a2,a1}); break;
    case D3DTOP_MODULATEINVCOLOR_ADDALPHA:
        e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,11),{c(0,rep(1)),neg(a1)});
        e.op(D3DSIO_MAD,d,{r(11),a2,r(9,rep(3))}); break;
    case D3DTOP_BUMPENVMAP:
    case D3DTOP_BUMPENVMAPLUMINANCE: break;
    case D3DTOP_DOTPRODUCT3:
        e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,11),{a1,neg(c(0,rep(3)))});
        e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,12),{a2,neg(c(0,rep(3)))});
        e.op(D3DSIO_DP3,dst(D3DSPR_TEMP,11),{r(11),r(12)});
        e.op(D3DSIO_MUL,d,{r(11),c(2,0)}); break;
    case D3DTOP_MULTIPLYADD: e.op(D3DSIO_MAD,d,{a1,a2,a0}); break;
    case D3DTOP_LERP: e.op(D3DSIO_LRP,d,{a0,a1,a2}); break;
    default: return false;
    }
    return true;
}
}

HRESULT pixel(const State &s, Program &p) {
    p.fixedFunction = true;
    try {
        p.constants.assign(PixelConstantCount, Constant{});
        p.constants[0]={0,1,2,.5f}; p.constants[1]=color(s.render[D3DRS_TEXTUREFACTOR]);
        p.constants[2]={4,0,0,0};
        p.tokens.clear(); p.tokens.push_back(D3DPS_VERSION(3,0)); Emit e{p.tokens};
        if(s.diffuse) e.dcl(D3DDECLUSAGE_COLOR,0,D3DSPR_INPUT,0);
        if(s.specular) e.dcl(D3DDECLUSAGE_COLOR,1,D3DSPR_INPUT,1);
        unsigned count=0;
        for(;count<StageCount;++count) {
            const auto &t=s.stages[count];
            if(t.colorOp==D3DTOP_DISABLE || (!t.bound && textureUsed(t,true))) break;
            if(t.colorOp< D3DTOP_DISABLE || t.colorOp>D3DTOP_LERP || t.alphaOp<D3DTOP_DISABLE || t.alphaOp>D3DTOP_LERP)
                return E_INVALIDARG;
            if(t.bound) {
                unsigned coord=s.transformed ? t.texcoord & 0xffff : count;
                if(coord<StageCount && (s.texcoords&(1u<<coord))) e.dcl(D3DDECLUSAGE_TEXCOORD,coord,D3DSPR_INPUT,count+2);
                e.v.insert(e.v.end(),{D3DSIO_DCL | (2<<D3DSI_INSTLENGTH_SHIFT),
                    0x80000000u | t.textureType, dst(D3DSPR_SAMPLER,count)});
            }
            p.constants[8+count]=color(t.constant);
            std::copy(t.bump,t.bump+4,p.constants[16+count].begin());
            p.constants[24+count]={t.bumpScale,t.bumpOffset,0,0};
        }
        e.mov(dst(D3DSPR_TEMP,0),s.diffuse ? src(D3DSPR_INPUT,0) : c(0,rep(1)));
        e.mov(dst(D3DSPR_TEMP,1),c(0,0));
        for(unsigned stage=0;stage<count;++stage) {
            const auto &t=s.stages[stage];
            const Stage *previous=stage ? &s.stages[stage-1] : nullptr;
            e.mov(dst(D3DSPR_TEMP,2),c(0,0));
            e.mov(dst(D3DSPR_TEMP,7),r(0));
            if(t.bound) {
                unsigned coord=s.transformed ? t.texcoord & 0xffff : stage;
                e.mov(dst(D3DSPR_TEMP,4),(coord<StageCount && (s.texcoords&(1u<<coord))) ? src(D3DSPR_INPUT,stage+2) : c(0,0));
                if(t.transform & D3DTTFF_PROJECTED) {
                    e.op(D3DSIO_RCP,dst(D3DSPR_TEMP,4,W),{r(4,rep(3))});
                    e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,4,XYZ),{r(4),r(4,rep(3))});
                }
                if(previous && (previous->colorOp==D3DTOP_BUMPENVMAP || previous->colorOp==D3DTOP_BUMPENVMAPLUMINANCE)) {
                    // Bump matrix uses row-vector convention: du'=du*m00+dv*m10.
                    e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,5,X|Y),{r(6,0),c(16+stage-1,0x44)});
                    e.op(D3DSIO_MAD,dst(D3DSPR_TEMP,5,X|Y),{r(6,rep(1)),c(16+stage-1,0xee),r(5)});
                    e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,4,X|Y),{r(4),r(5)});
                }
                e.op(D3DSIO_TEX,dst(D3DSPR_TEMP,2),{r(4),src(D3DSPR_SAMPLER,stage)});
                if(previous && previous->colorOp==D3DTOP_BUMPENVMAPLUMINANCE) {
                    e.op(D3DSIO_MAD,dst(D3DSPR_TEMP,5,X,true),{r(6,rep(2)),c(24+stage-1,0),c(24+stage-1,rep(1))});
                    e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,2),{r(2),r(5,0)});
                }
                if(previous && previous->colorOp==D3DTOP_PREMODULATE)
                    e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,7,XYZ),{r(0),r(2)});
                if(previous && previous->alphaOp==D3DTOP_PREMODULATE)
                    e.op(D3DSIO_MUL,dst(D3DSPR_TEMP,7,W),{r(0),r(2)});
            }
            if(t.result!=D3DTA_CURRENT && t.result!=D3DTA_TEMP) return E_INVALIDARG;
            e.mov(dst(D3DSPR_TEMP,3),r(t.result==D3DTA_TEMP ? 1 : 0));
            for(unsigned channel=0;channel<2;++channel) {
                unsigned op=channel ? t.alphaOp : t.colorOp;
                for(unsigned arg=0;arg<3;++arg) {
                    if(usesArg(op,arg)) {
                        unsigned argument=channel ? t.alphaArg[arg] : t.colorArg[arg];
                        // With no texture, ALPHAARG1 uses vertex diffuse alpha.
                        if(channel && arg==1 && !t.bound &&
                           (argument&D3DTA_SELECTMASK)==D3DTA_TEXTURE)
                            argument=(argument&~D3DTA_SELECTMASK)|D3DTA_DIFFUSE;
                        if(!loadArg(e,s,stage,argument,8+arg)) return E_INVALIDARG;
                    }
                }
                if(!operation(e,s,op,channel ? W : XYZ)) return E_INVALIDARG;
            }
            e.mov(dst(D3DSPR_TEMP,t.result==D3DTA_TEMP ? 1 : 0),r(3));
            e.mov(dst(D3DSPR_TEMP,6),r(2));
        }
        if(s.render[D3DRS_SPECULARENABLE] && s.specular)
            e.op(D3DSIO_ADD,dst(D3DSPR_TEMP,0,XYZ,true),{r(0),src(D3DSPR_INPUT,1)});
        e.mov(dst(D3DSPR_COLOROUT,0),r(0)); p.tokens.push_back(D3DPS_END());
    } catch (...) { return E_OUTOFMEMORY; }
    return S_OK;
}
}
