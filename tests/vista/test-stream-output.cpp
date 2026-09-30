/* SPDX-License-Identifier: MIT
 * Actual native GPU readbacks for production D3D10 stream-output aliases. */
#include <d3d11.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

extern "C" void *tritonBuildDxbc(const UINT *, SIZE_T, const void *, UINT,
    const void *, UINT, const void *, UINT, UINT, SIZE_T *);
extern "C" void *tritonD3D10BuildSOAlias(const void *, UINT, UINT, SIZE_T *);

template<typename T> struct Com {
    T *p = nullptr;
    ~Com() { if (p) p->Release(); }
    T *operator->() const { return p; }
    T **put() { return &p; }
};
static void hr(HRESULT value, const char *expr) {
    if (FAILED(value)) { std::printf("FAIL %s hr=%08x\n", expr, unsigned(value)); throw std::runtime_error(expr); }
}
#define HR(x) hr(x, #x)
static unsigned checks, failures;
static bool baseline;
static void expect(bool value, const char *label) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", label); }
}
struct Signature { UINT value, reg; BYTE mask, pad[3]; };
using Vertex = std::array<float, 4>;
struct Case {
    const char *name;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT count, primitiveSize;
    std::vector<UINT> expected;
    std::vector<unsigned short> indices;
    UINT instances = 1;
};

struct Test {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<ID3D11VertexShader> vs;
    Com<ID3D11InputLayout> layout;
    Com<ID3D11Buffer> input;
    Com<ID3D11GeometryShader> alias[2];
    std::array<Vertex, 32> vertices;

    Test() {
        D3D_FEATURE_LEVEL level;
        HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, device.put(), &level, context.put()));
        // vs_4_0: dcl_input v0; dcl_output_siv o0,position; mov o0,v0; ret.
        const UINT tokens[] = {0x10040,23,0x0300005f,0x001010f2,0,
            0x04000067,0x001020f2,0,1,0x03000065,0x001020f2,1,
            0x05000036,0x001020f2,0,0x00101e46,0,
            0x05000036,0x001020f2,1,0x001011b6,0,0x0100003e};
        Signature in = {0,0,15,{}};
        Signature out[] = {{1,0,15,{}},{0,1,15,{}}};
        SIZE_T size = 0;
        void *code = tritonBuildDxbc(tokens,sizeof(tokens),&in,1,out,2,nullptr,0,sizeof(in),&size);
        if (!code) throw std::runtime_error("VS container");
        HR(device->CreateVertexShader(code,size,nullptr,vs.put()));
        D3D11_INPUT_ELEMENT_DESC element = {"ATTRIB",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
        HR(device->CreateInputLayout(&element,1,code,size,layout.put()));
        std::free(code);
        code = tritonD3D10BuildSOAlias(out,2,sizeof(out[0]),&size);
        if (!code) throw std::runtime_error("SO alias container");
        D3D11_SO_DECLARATION_ENTRY entry = {0,"SV_Position",0,0,4,0};
        UINT stride = sizeof(Vertex);
        for (UINT i=0;i<2;++i)
            HR(device->CreateGeometryShaderWithStreamOutput(code,size,&entry,1,&stride,1,
                i ? D3D11_SO_NO_RASTERIZED_STREAM : 0,nullptr,alias[i].put()));
        std::free(code);
        for (UINT i=0;i<vertices.size();++i)
            vertices[i] = {float(i)/32.f, float(i+1)/64.f, float(i+2)/128.f, 1.f};
        D3D11_BUFFER_DESC desc = {sizeof(vertices),D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER,0,0,0};
        D3D11_SUBRESOURCE_DATA data = {vertices.data(),0,0};
        HR(device->CreateBuffer(&desc,&data,input.put()));
    }
    ~Test() { context->ClearState(); }

    void wait(ID3D11Asynchronous *query, void *data, UINT bytes) {
        auto start = std::chrono::steady_clock::now();
        for (;;) {
            HRESULT result = context->GetData(query,data,bytes,0);
            if (result != S_FALSE) { HR(result); return; }
            if (std::chrono::steady_clock::now()-start > std::chrono::seconds(10))
                throw std::runtime_error("query timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void buffer(UINT count, UINT binds, Com<ID3D11Buffer> &out) {
        D3D11_BUFFER_DESC desc = {count*UINT(sizeof(Vertex)),D3D11_USAGE_DEFAULT,binds,0,0,0};
        HR(device->CreateBuffer(&desc,nullptr,out.put()));
    }
    std::vector<Vertex> read(ID3D11Buffer *buffer) {
        D3D11_BUFFER_DESC desc; buffer->GetDesc(&desc);
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Com<ID3D11Buffer> staging;HR(device->CreateBuffer(&desc,nullptr,staging.put()));
        context->CopyResource(staging.p,buffer);
        D3D11_MAPPED_SUBRESOURCE map;HR(context->Map(staging.p,0,D3D11_MAP_READ,0,&map));
        std::vector<Vertex> result(desc.ByteWidth/sizeof(Vertex));
        std::memcpy(result.data(),map.pData,desc.ByteWidth);context->Unmap(staging.p,0);
        return result;
    }
    void compare(const std::vector<Vertex> &got, const std::vector<UINT> &expected,
                 const char *label, UINT offset=0) {
        bool same = true;
        for (UINT i=0;i<expected.size();++i) {
            if (std::memcmp(&got.at(offset+i),&vertices.at(expected[i]),sizeof(Vertex))) {
                std::printf("DATA %s vertex=%u got=%g expected=%g\n",label,i,got.at(offset+i)[0],vertices.at(expected[i])[0]);
                same=false;break;
            }
        }
        expect(same,label);
    }
    void packed() {
        context->ClearState();
        Signature sigs[]={{1,0,15,{}},{0,1,15,{}}};SIZE_T size=0;
        void *code=tritonD3D10BuildSOAlias(sigs,2,sizeof(sigs[0]),&size);
        // Capture Position.xy to buffer0 and the reversed generic output.zw
        // to buffer1. Extra stride bytes must remain untouched.
        D3D11_SO_DECLARATION_ENTRY decl[]={{0,"SV_Position",0,0,2,0},{0,"ATTRIB",0,2,2,1}};
        UINT strides[]={16,16};Com<ID3D11GeometryShader> gs;
        HR(device->CreateGeometryShaderWithStreamOutput(code,size,decl,2,strides,2,
            D3D11_SO_NO_RASTERIZED_STREAM,nullptr,gs.put()));std::free(code);
        Com<ID3D11Buffer> outputs[2];
        std::vector<Vertex> sentinel(7,Vertex{-7,-7,-7,-7});
        for (auto &output:outputs) {
            buffer(7,D3D11_BIND_STREAM_OUTPUT,output);
            context->UpdateSubresource(output.p,0,nullptr,sentinel.data(),0,0);
        }
        UINT stride=sizeof(Vertex),offset=0;
        context->IASetInputLayout(layout.p);context->IASetVertexBuffers(0,1,&input.p,&stride,&offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.p,nullptr,0);context->GSSetShader(gs.p,nullptr,0);
        ID3D11Buffer *buffers[]={outputs[0].p,outputs[1].p};UINT offsets[]={0,0};
        context->SOSetTargets(2,buffers,offsets);context->Draw(6,0);context->SOSetTargets(0,nullptr,nullptr);
        auto a=read(outputs[0].p),b=read(outputs[1].p);
        bool exact=true;
        for (UINT i=0;i<6;++i) {
            exact &= a[i]==Vertex{vertices[i][0],vertices[i][1],-7,-7};
            exact &= b[i]==Vertex{vertices[i][1],vertices[i][0],-7,-7};
        }
        expect(exact,"multiple output registers, partial components, separate buffers and stride padding");
        expect(a.back()==sentinel.back() && b.back()==sentinel.back(),"multiple buffers retain unwritten tail");
    }
    void append() {
        context->ClearState();
        Com<ID3D11Buffer> output,replay;buffer(8,D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_VERTEX_BUFFER,output);
        buffer(8,D3D11_BIND_STREAM_OUTPUT,replay);
        std::array<Vertex,8> sentinel;sentinel.fill(Vertex{-7,-7,-7,-7});
        context->UpdateSubresource(output.p,0,nullptr,sentinel.data(),0,0);
        UINT stride=sizeof(Vertex),offset=0;
        context->IASetInputLayout(layout.p);context->IASetVertexBuffers(0,1,&input.p,&stride,&offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.p,nullptr,0);context->GSSetShader(alias[1].p,nullptr,0);
        offset=sizeof(Vertex);context->SOSetTargets(1,&output.p,&offset);context->Draw(3,0);
        context->SOSetTargets(0,nullptr,nullptr);
        offset=~0u;context->SOSetTargets(1,&output.p,&offset);context->Draw(3,3);
        context->SOSetTargets(0,nullptr,nullptr);
        const std::vector<UINT> expected={0,1,2,3,4,5};
        auto got=read(output.p);compare(got,expected,"SO append retains both draws",1);
        expect(got.front()==sentinel.front() && got.back()==sentinel.back(),"append preserves offset and unwritten tail");
        offset=sizeof(Vertex);context->IASetVertexBuffers(0,1,&output.p,&stride,&offset);
        offset=0;context->SOSetTargets(1,&replay.p,&offset);
        D3D11_QUERY_DESC desc={D3D11_QUERY_SO_STATISTICS,0};Com<ID3D11Query> query;
        HR(device->CreateQuery(&desc,query.put()));context->Begin(query.p);context->DrawAuto();context->End(query.p);
        context->SOSetTargets(0,nullptr,nullptr);D3D11_QUERY_DATA_SO_STATISTICS stats={};wait(query.p,&stats,sizeof(stats));
        expect(stats.NumPrimitivesWritten==2 && stats.PrimitivesStorageNeeded==2,"DrawAuto after append preserves primitive count");
        compare(read(replay.p),expected,"DrawAuto after append contents");
    }
    void rasterize(const Case &test) {
        context->ClearState();
        // Distinct vertices on a spiral produce nondegenerate interior triangles.
        for (UINT i=0;i<vertices.size();++i) {
            float radius=.35f+.012f*i;
            vertices[i]={radius*std::cos(.83f*i),radius*std::sin(.83f*i),.5f,1.f};
        }
        context->UpdateSubresource(input.p,0,nullptr,vertices.data(),0,0);
        const UINT tokens[]={0x40,14,0x03000065,0x001020f2,0,
            0x08000036,0x001020f2,0,0x00004002,0x3f800000,0x3e800000,0x3f000000,0x3f800000,0x0100003e};
        Signature out={0,0,15,{}};SIZE_T size=0;
        void *code=tritonBuildDxbc(tokens,sizeof(tokens),nullptr,0,&out,1,nullptr,0,sizeof(out),&size);
        Com<ID3D11PixelShader> ps;HR(device->CreatePixelShader(code,size,nullptr,ps.put()));std::free(code);
        D3D11_TEXTURE2D_DESC desc={32,32,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};
        Com<ID3D11Texture2D> render,staging;Com<ID3D11RenderTargetView> rtv;
        HR(device->CreateTexture2D(&desc,nullptr,render.put()));
        HR(device->CreateRenderTargetView(render.p,nullptr,rtv.put()));
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        HR(device->CreateTexture2D(&desc,nullptr,staging.put()));
        D3D11_RASTERIZER_DESC raster={};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
        Com<ID3D11RasterizerState> rs;HR(device->CreateRasterizerState(&raster,rs.put()));
        context->RSSetState(rs.p);D3D11_VIEWPORT viewport={0,0,32,32,0,1};context->RSSetViewports(1,&viewport);
        context->OMSetRenderTargets(1,&rtv.p,nullptr);context->PSSetShader(ps.p,nullptr,0);
        context->IASetInputLayout(layout.p);context->IASetPrimitiveTopology(test.topology);
        UINT stride=sizeof(Vertex),offset=0;context->IASetVertexBuffers(0,1,&input.p,&stride,&offset);
        context->VSSetShader(vs.p,nullptr,0);
        Com<ID3D11Buffer> index,output;buffer(64,D3D11_BIND_STREAM_OUTPUT,output);
        if (!test.indices.empty()) {
            D3D11_BUFFER_DESC bd={UINT(test.indices.size()*sizeof(unsigned short)),D3D11_USAGE_DEFAULT,D3D11_BIND_INDEX_BUFFER,0,0,0};
            D3D11_SUBRESOURCE_DATA data={test.indices.data(),0,0};HR(device->CreateBuffer(&bd,&data,index.put()));
            context->IASetIndexBuffer(index.p,DXGI_FORMAT_R16_UINT,0);
        }
        std::array<std::vector<UINT>,2> pixels;
        for (UINT enabled=0;enabled<2;++enabled) {
            const float clear[4]={0,0,0,0};context->ClearRenderTargetView(rtv.p,clear);
            context->GSSetShader(enabled?alias[0].p:nullptr,nullptr,0);
            context->SOSetTargets(enabled?1:0,enabled?&output.p:nullptr,enabled?&offset:nullptr);
            if (test.indices.empty()) context->DrawInstanced(test.count,test.instances,0,0);
            else context->DrawIndexedInstanced(test.count,test.instances,0,0,0);
            context->SOSetTargets(0,nullptr,nullptr);context->CopyResource(staging.p,render.p);
            D3D11_MAPPED_SUBRESOURCE map;HR(context->Map(staging.p,0,D3D11_MAP_READ,0,&map));
            pixels[enabled].resize(32*32);
            for (UINT y=0;y<32;++y)std::memcpy(pixels[enabled].data()+32*y,static_cast<const BYTE*>(map.pData)+y*map.RowPitch,32*4);
            context->Unmap(staging.p,0);
        }
        std::printf("RASTER %s pixels=%zu\n",test.name,
            size_t(std::count_if(pixels[0].begin(),pixels[0].end(),[](UINT p){return p!=0;})));
        expect(std::any_of(pixels[0].begin(),pixels[0].end(),[](UINT p){return p!=0;}),"reference raster draws pixels");
        expect(pixels[0]==pixels[1],"stream output preserves rasterized primitive coverage");
    }

    void run(const Case &test, UINT raster, UINT capacity, UINT soStart=0) {
        context->ClearState();
        Com<ID3D11Buffer> output,index;
        buffer(capacity,D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_VERTEX_BUFFER,output);
        std::vector<Vertex> sentinel(capacity,Vertex{-7,-7,-7,-7});
        context->UpdateSubresource(output.p,0,nullptr,sentinel.data(),0,0);
        UINT stride=sizeof(Vertex),offset=0;
        context->IASetInputLayout(layout.p);context->IASetVertexBuffers(0,1,&input.p,&stride,&offset);
        context->IASetPrimitiveTopology(test.topology);context->VSSetShader(vs.p,nullptr,0);
        context->GSSetShader(alias[raster].p,nullptr,0);
        offset=soStart*sizeof(Vertex);context->SOSetTargets(1,&output.p,&offset);
        if (!test.indices.empty()) {
            D3D11_BUFFER_DESC desc={UINT(test.indices.size()*sizeof(unsigned short)),D3D11_USAGE_DEFAULT,D3D11_BIND_INDEX_BUFFER,0,0,0};
            D3D11_SUBRESOURCE_DATA data={test.indices.data(),0,0};
            HR(device->CreateBuffer(&desc,&data,index.put()));
            context->IASetIndexBuffer(index.p,DXGI_FORMAT_R16_UINT,0);
        }
        Com<ID3D11Query> statistics;Com<ID3D11Predicate> overflow;
        D3D11_QUERY_DESC query={D3D11_QUERY_SO_STATISTICS,0};
        HR(device->CreateQuery(&query,statistics.put()));
        query.Query=D3D11_QUERY_SO_OVERFLOW_PREDICATE;
        HRESULT predicateResult=device->CreatePredicate(&query,overflow.put());
        if (!baseline || SUCCEEDED(predicateResult)) HR(predicateResult);
        context->Begin(statistics.p);if (overflow.p) context->Begin(overflow.p);
        if (test.indices.empty()) context->DrawInstanced(test.count,test.instances,0,0);
        else context->DrawIndexedInstanced(test.count,test.instances,0,0,0);
        if (overflow.p) context->End(overflow.p);
        context->End(statistics.p);
        context->SOSetTargets(0,nullptr,nullptr);
        D3D11_QUERY_DATA_SO_STATISTICS stats={};BOOL overflowed=FALSE;
        wait(statistics.p,&stats,sizeof(stats));if (overflow.p) wait(overflow.p,&overflowed,sizeof(overflowed));
        std::vector<UINT> expected;
        for (UINT i=0;i<test.instances;++i)expected.insert(expected.end(),test.expected.begin(),test.expected.end());
        UINT needed=expected.size()/test.primitiveSize;
        UINT written=std::min(needed,(capacity-soStart)/test.primitiveSize);
        std::printf("CASE %s raster=%u capacity=%u written=%llu needed=%llu overflow=%d\n",
            test.name,!raster,capacity,(unsigned long long)stats.NumPrimitivesWritten,
            (unsigned long long)stats.PrimitivesStorageNeeded,int(overflowed));
        expect(stats.NumPrimitivesWritten==written && stats.PrimitivesStorageNeeded==needed,"SO primitive statistics");
        if (overflow.p) expect(bool(overflowed)==(written<needed),"SO overflow predicate");
        expected.resize(written*test.primitiveSize);
        auto got=read(output.p);compare(got,expected,test.name,soStart);
        expect(std::equal(got.begin(),got.begin()+soStart,sentinel.begin()),"SO byte offset leaves prefix untouched");
        expect(std::equal(got.begin()+soStart+expected.size(),got.end(),sentinel.begin()+soStart+expected.size()),"incomplete primitive leaves tail untouched");

        // Reuse the hidden written-byte counter through DrawAuto, with and
        // without an IA byte offset. A point list makes its vertex count exact.
        for (UINT skip : {0u,1u,UINT(expected.size()),std::min(capacity-soStart,UINT(expected.size()+1))}) {
            Com<ID3D11Buffer> replay;buffer(capacity+1,D3D11_BIND_STREAM_OUTPUT,replay);
            offset=(soStart+skip)*sizeof(Vertex);context->IASetVertexBuffers(0,1,&output.p,&stride,&offset);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            offset=0;context->SOSetTargets(1,&replay.p,&offset);
            context->Begin(statistics.p);context->DrawAuto();context->End(statistics.p);
            context->SOSetTargets(0,nullptr,nullptr);wait(statistics.p,&stats,sizeof(stats));
            std::vector<UINT> replayExpected(expected.begin()+std::min<size_t>(skip,expected.size()),expected.end());
            if (stats.NumPrimitivesWritten!=replayExpected.size() || stats.PrimitivesStorageNeeded!=replayExpected.size())
                std::printf("DATA DrawAuto skip=%u written=%llu needed=%llu expected=%zu\n",skip,
                    (unsigned long long)stats.NumPrimitivesWritten,(unsigned long long)stats.PrimitivesStorageNeeded,replayExpected.size());
            expect(stats.NumPrimitivesWritten==replayExpected.size() && stats.PrimitivesStorageNeeded==replayExpected.size(),"DrawAuto vertex count and IA offset");
            compare(read(replay.p),replayExpected,"DrawAuto contents");
        }
    }
};

int main(int argc, char **) {
    baseline = argc > 1;
    try {
        Test test;
        std::vector<Case> cases = {
            {"points",D3D11_PRIMITIVE_TOPOLOGY_POINTLIST,7,1,{0,1,2,3,4,5,6},{}},
            {"lines",D3D11_PRIMITIVE_TOPOLOGY_LINELIST,7,2,{0,1,2,3,4,5},{}},
            {"line strip",D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,5,2,{0,1,1,2,2,3,3,4},{}},
            {"triangles",D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,8,3,{0,1,2,3,4,5},{}},
            {"triangle strip",D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,5,3,{0,1,2,1,3,2,2,3,4},{}},
            {"lines adjacent",D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ,9,2,{1,2,5,6},{}},
            {"line strip adjacent",D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ,6,2,{1,2,2,3,3,4},{}},
            {"triangles adjacent",D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ,13,3,{0,2,4,6,8,10},{}},
            {"triangle strip adjacent",D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ,10,3,{0,2,4,2,6,4,4,6,8},{}},
            {"line restart",D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,7,2,{0,1,1,2,4,5,5,6},{0,1,2,65535,4,5,6}},
            {"triangle restart",D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,9,3,{0,1,2,1,3,2,5,6,7,6,8,7},{0,1,2,3,65535,5,6,7,8}},
            {"instances",D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,3,3,{0,1,2},{},2},
        };
        for (UINT raster=0;raster<2;++raster) {
            for (const auto &item : cases) {
                test.run(item,raster,32);
                test.run(item,raster,item.primitiveSize+1);
                test.run(item,raster,std::max(1u,item.primitiveSize-1));
                test.run(item,raster,32,1);
            }
            std::reverse(cases.begin(),cases.end());
        }
        test.packed();
        test.append();
        for (const auto &item : cases)test.rasterize(item);
        std::printf("Stream-output checks: %u passed, %u failed\n",checks-failures,failures);
        return failures ? 1 : 0;
    } catch (const std::exception &e) {
        std::printf("FAIL exception: %s\n",e.what());return 1;
    }
}
