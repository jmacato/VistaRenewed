// SPDX-License-Identifier: MIT
// Execute emitted SM2 instructions against independent row-vector matrix results.
#include "triton9_fixed.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
using namespace Triton9Fixed;
struct HostBuffer { void Release() { assert(false); } };
struct Buffer { unsigned char *data{}; UINT byteCount{}; BOOL dirty{}; HostBuffer *hostBuffer{}; };
struct TRITON9_DEVICE { Buffer fixedVertexFloatConstants, fixedPixelFloatConstants; };
static void *GetProcessHeap() { return nullptr; }
static void HeapFree(void *, unsigned, void *p) { free(p); }
static UINT triton9ConstantBufferSize(BOOL, UINT) { return PixelConstantCount * 16; }
static HRESULT triton9EnsureConstantData(Buffer *b, UINT size) {
    if (!b->data) { b->data = static_cast<unsigned char *>(calloc(1, size)); b->byteCount = size; }
    return b->data ? S_OK : E_OUTOFMEMORY;
}
/* SOURCE_UNDER_TEST */
struct Machine {
    Constant input[16]{}, temp[32]{}, output[8]{}, raster[3]{}, attr[2]{};
    const Program &p;
    static unsigned kind(unsigned t) { return ((t&D3DSP_REGTYPE_MASK)>>D3DSP_REGTYPE_SHIFT)|((t&D3DSP_REGTYPE_MASK2)>>D3DSP_REGTYPE_SHIFT2); }
    Constant &reg(unsigned t) {
        unsigned n=t&D3DSP_REGNUM_MASK;
        switch(kind(t)) {
        case D3DSPR_INPUT: assert(n<16); return input[n];
        case D3DSPR_TEMP: assert(n<32); return temp[n];
        case D3DSPR_TEXCRDOUT: assert(n<8); return output[n];
        case D3DSPR_RASTOUT: assert(n<3); return raster[n];
        case D3DSPR_ATTROUT: assert(n<2); return attr[n];
        default: assert(false); return temp[0];
        }
    }
    Constant read(unsigned t) {
        Constant base=kind(t)==D3DSPR_CONST?p.constants.at(t&D3DSP_REGNUM_MASK):reg(t), value{};
        for(unsigned k=0;k<4;++k) value[k]=base[(t>>(16+k*2))&3];
        unsigned modifier=t&D3DSP_SRCMOD_MASK;
        assert(!modifier || modifier==D3DSPSM_NEG);
        if(modifier) for(auto &v:value) v=-v;
        return value;
    }
    void run() {
        for(size_t i=1;i<p.tokens.size();) {
            unsigned inst=p.tokens[i++], op=inst&D3DSI_OPCODE_MASK;
            if(op==D3DSIO_END) { assert(i==p.tokens.size()); return; }
            unsigned len=(inst&D3DSI_INSTLENGTH_MASK)>>D3DSI_INSTLENGTH_SHIFT;
            if(op==D3DSIO_DCL) { assert(len==2); i+=len; continue; }
            unsigned d=p.tokens.at(i++); Constant a=read(p.tokens.at(i++)), b{}, c{}, result{};
            if(len>=3) b=read(p.tokens.at(i++));
            if(len==4) c=read(p.tokens.at(i++));
            assert(len>=2 && len<=4);
            for(unsigned k=0;k<4;++k) {
                switch(op) {
                case D3DSIO_MOV: result[k]=a[k]; break;
                case D3DSIO_MAD: result[k]=a[k]*b[k]+c[k]; break;
                case D3DSIO_MAX: result[k]=std::max(a[k],b[k]); break;
                case D3DSIO_MIN: result[k]=std::min(a[k],b[k]); break;
                case D3DSIO_M4x4: case D3DSIO_M3x3: {
                    unsigned n=op==D3DSIO_M4x4?4:3;
                    if(k<n) { auto row=read(p.tokens[i-1]+k); for(unsigned j=0;j<n;++j) result[k]+=a[j]*row[j]; }
                    break;
                }
                default: assert(false);
                }
                if(d&(D3DSP_WRITEMASK_0<<k)) reg(d)[k]=result[k];
            }
        }
        assert(false);
    }
};
int main() {
    unsigned cases=0;
    for(unsigned stage=0;stage<8;++stage) for(unsigned width=1;width<=4;++width)
    for(unsigned count=0;count<=4;++count) for(unsigned projected=0;projected<=1;++projected) {
        if(projected && count<2) continue;
        State s{};
        for(unsigned j=0;j<4;++j) s.world[0].m[j][j]=s.view.m[j][j]=s.projection.m[j][j]=1;
        s.inputs={{D3DDECLUSAGE_POSITION,0,0,4},{D3DDECLUSAGE_TEXCOORD,5,3,width}};
        for(unsigned i=0;i<8;++i) {
            s.stages[i].texcoord=5;
            for(unsigned row=0;row<4;++row) for(unsigned col=0;col<4;++col)
                s.texture[i].m[row][col]=float(1+row*4+col+i*16)/16;
        }
        s.stages[stage].transform=count|(projected?D3DTTFF_PROJECTED:0);
        Program p; assert(vertex(s,p)==S_OK);
        Machine m{{},{},{},{},{},p}; m.input[0]={0,0,.5f,1}; m.input[3]={.25f,.75f,-.5f,.9f}; m.run();
        Constant padded=m.input[3];
        for(unsigned k=width;k<4;++k) padded[k]=k==width?1:0;
        for(unsigned i=0;i<8;++i) {
            Constant expected{};
            if(i==stage && count) {
                for(unsigned k=0;k<count;++k) for(unsigned j=0;j<4;++j) expected[k]+=padded[j]*s.texture[i].m[j][k];
                if(projected) expected[3]=expected[count-1];
            } else for(unsigned k=0;k<width;++k) expected[k]=m.input[3][k];
            for(unsigned k=0;k<4;++k) assert(std::fabs(m.output[i][k]-expected[k])<1e-6f);
        }
        auto same=s; assert(equal(s,same));
        TRITON9_DEVICE device{};
        assert(triton9UploadFixedProgram(&device,TRUE,p)==S_OK);
        assert(device.fixedVertexFloatConstants.dirty);
        device.fixedVertexFloatConstants.dirty=FALSE;
        assert(triton9UploadFixedProgram(&device,TRUE,p)==S_OK);
        assert(!device.fixedVertexFloatConstants.dirty);
        same.texture[stage].m[2][0]+=.5f; assert(!equal(s,same));
        Program changed; assert(vertex(same,changed)==S_OK);
        assert(changed.tokens==p.tokens); assert(changed.constants!=p.constants);
        assert(triton9UploadFixedProgram(&device,TRUE,changed)==S_OK);
        assert(device.fixedVertexFloatConstants.dirty);
        assert(!memcmp(device.fixedVertexFloatConstants.data,changed.constants.data(),changed.constants.size()*sizeof(Constant)));
        free(device.fixedVertexFloatConstants.data); ++cases;
    }
    printf("texture transform: %u full production vertex programs passed; eight stages, input widths 1-4, disabled/count1-4/projected, matrix update and upload dirty tracking\n",cases);
}
