/* SPDX-License-Identifier: MIT
 * Public D3D11 native predication oracle. Every observation disables predication
 * so a skipped readback cannot disguise an incorrect rendering result. */
#define TRITON_LINUX_D3D11_NO_MAIN
#include "../linux-d3d11-test.cpp"
#include <array>
static bool dropPredicate = false;
static void pred(ID3D11DeviceContext* ctx, ID3D11Predicate* p, BOOL value) {
  ctx->SetPredication(dropPredicate ? nullptr : p, value);
}
struct Target {
  Ctx& c;
  Com<ID3D11Texture2D> texture;
  Com<ID3D11RenderTargetView> view;
  Com<ID3D11Buffer> vertices,indices,indirect,indexedIndirect;
  Com<ID3D11VertexShader> vs;
  Com<ID3D11PixelShader> ps;
  Com<ID3D11InputLayout> layout;
  Com<ID3D11RasterizerState> raster;
  bool valid=false;
  Target(Ctx& context):c(context) {
    const char* T="predicate_setup";
    D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=16;td.MipLevels=td.ArraySize=1;
    td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    CHECK_HR(T,c.device->CreateTexture2D(&td,nullptr,&texture));
    CHECK_HR(T,c.device->CreateRenderTargetView(texture,nullptr,&view));
    CHECK_HR(T,c.device->CreateVertexShader(dxbc_vs_pass,sizeof(dxbc_vs_pass),nullptr,&vs));
    CHECK_HR(T,c.device->CreatePixelShader(dxbc_ps_color,sizeof(dxbc_ps_color),nullptr,&ps));
    CHECK_HR(T,c.device->CreateInputLayout(kLayout,3,dxbc_vs_pass,sizeof(dxbc_vs_pass),&layout));
    Vertex v[6];memcpy(v,kQuadFull,sizeof(v));for(auto& p:v){p.color[0]=1;p.color[1]=p.color[2]=0;p.color[3]=1;}
    D3D11_BUFFER_DESC bd={};bd.ByteWidth=sizeof(v);bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data={v,0,0};CHECK_HR(T,c.device->CreateBuffer(&bd,&data,&vertices));
    uint32_t idx[]={0,1,2,3,4,5};bd.ByteWidth=sizeof(idx);bd.BindFlags=D3D11_BIND_INDEX_BUFFER;data.pSysMem=idx;
    CHECK_HR(T,c.device->CreateBuffer(&bd,&data,&indices));
    uint32_t args[]={6,1,0,0,0};bd.ByteWidth=sizeof(args);bd.BindFlags=0;bd.MiscFlags=D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;data.pSysMem=args;
    CHECK_HR(T,c.device->CreateBuffer(&bd,&data,&indirect));CHECK_HR(T,c.device->CreateBuffer(&bd,&data,&indexedIndirect));
    D3D11_RASTERIZER_DESC rd={};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    CHECK_HR(T,c.device->CreateRasterizerState(&rd,&raster));valid=true;
  }
  void bind(ID3D11DeviceContext* ctx=nullptr) {
    if(!ctx)ctx=c.ctx;
    ctx->OMSetRenderTargets(1,&view,nullptr);D3D11_VIEWPORT vp={0,0,16,16,0,1};ctx->RSSetViewports(1,&vp);ctx->RSSetState(raster);
    UINT stride=sizeof(Vertex),offset=0;ctx->IASetVertexBuffers(0,1,&vertices,&stride,&offset);ctx->IASetIndexBuffer(indices,DXGI_FORMAT_R32_UINT,0);
    ctx->IASetInputLayout(layout);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs,nullptr,0);ctx->PSSetShader(ps,nullptr,0);
  }
  void green(ID3D11DeviceContext* ctx=nullptr){if(!ctx)ctx=c.ctx;const float g[]={0,1,0,1};ctx->ClearRenderTargetView(view,g);}
  bool isRed(bool red) {
    Com<ID3D11Predicate> saved;BOOL value=FALSE;c.ctx->GetPredication(&saved,&value);c.ctx->SetPredication(nullptr,FALSE);
    auto pixels=readback_tex2d(c,texture,0,4);c.ctx->SetPredication(saved,value);
    return pixels.size()==1024&&px_eq(pixels,16,8,8,red?255:0,red?0:255,0,255,0);
  }
  void draw(unsigned kind,ID3D11DeviceContext* ctx=nullptr) {
    if(!ctx)ctx=c.ctx;
    switch(kind){case 0:ctx->Draw(6,0);break;case 1:ctx->DrawIndexed(6,0,0);break;
    case 2:ctx->DrawInstanced(6,1,0,0);break;case 3:ctx->DrawIndexedInstanced(6,1,0,0,0);break;
    case 4:ctx->DrawInstancedIndirect(indirect,0);break;case 5:ctx->DrawIndexedInstancedIndirect(indexedIndirect,0);break;}
  }
};
static ID3D11Predicate* issue(Ctx& c,Target& t,bool result, bool consume=false,ID3D11DeviceContext* ctx=nullptr) {
  const char* T="predicate_issue";if(!ctx)ctx=c.ctx;
  ID3D11Predicate* p=nullptr;D3D11_QUERY_DESC q={D3D11_QUERY_OCCLUSION_PREDICATE,0};
  CHECK(T,c.device->CreatePredicate(&q,&p)==S_OK,return nullptr);
  ctx->SetPredication(nullptr,FALSE);t.bind(ctx);ctx->Begin(p);
  if(result){ctx->Draw(6,0);ctx->Flush();ctx->Draw(6,0);}ctx->End(p);
  if(consume){BOOL actual=FALSE;HRESULT hr=S_FALSE;for(unsigned i=0;i<10000&&hr==S_FALSE;i++){hr=c.ctx->GetData(p,&actual,sizeof(actual),0);if(hr==S_FALSE)usleep(100);}
    CHECK(T,hr==S_OK&&!!actual==result,return {});}
  return p;
}
static void draws(Ctx& c) {
  const char* T="predicate_draw";c.ctx->ClearState();Target t(c);CHECK(T,t.valid,return);
  for(bool consumed:{false,true})for(bool result:{false,true})for(BOOL value:{FALSE,TRUE}){
    Com<ID3D11Predicate> p{issue(c,t,result,consumed)};CHECK(T,p!=nullptr,return);
    for(unsigned kind=0;kind<6;kind++){
      c.ctx->SetPredication(nullptr,FALSE);t.green();t.bind();pred(c.ctx,p,value);t.draw(kind);
      bool expected=result!=bool(value);bool ok=t.isRed(expected);
      emit("[DRAW] consumed=%u result=%u compare=%u kind=%u correct=%u",consumed,result,value,kind,ok);CHECK(T,ok);
    }
  }
  c.ctx->SetPredication(nullptr,TRUE);t.green();t.draw(0);CHECK(T,t.isRed(true));
  Com<ID3D11Predicate> got;BOOL value=FALSE;c.ctx->GetPredication(&got,&value);CHECK(T,got==nullptr&&value==TRUE);
  c.ctx->ClearState();c.ctx->GetPredication(&got,&value);CHECK(T,got==nullptr&&value==FALSE);
}
static uint32_t word(Ctx& c,ID3D11Buffer* buffer) {
  Com<ID3D11Predicate> saved;BOOL value;c.ctx->GetPredication(&saved,&value);c.ctx->SetPredication(nullptr,FALSE);
  auto data=readback_buffer(c,buffer);c.ctx->SetPredication(saved,value);uint32_t v=~0u;if(data.size()>=4)memcpy(&v,data.data(),4);return v;
}
static void resources(Ctx& c) {
  const char* T="predicate_resource";c.ctx->ClearState();Target t(c);CHECK(T,t.valid,return);
  Com<ID3D11DeviceContext1> ctx1;CHECK_HR(T,c.ctx->QueryInterface(__uuidof(ID3D11DeviceContext1),reinterpret_cast<void**>(&ctx1)));
  D3D11_BUFFER_DESC bd={};bd.ByteWidth=128;uint32_t original[32],changed[32];for(unsigned i=0;i<32;i++){original[i]=0xabcdef01;changed[i]=0x12345678;}
  D3D11_SUBRESOURCE_DATA data={changed,0,0};Com<ID3D11Buffer> source,buffer,dynamic;
  CHECK_HR(T,c.device->CreateBuffer(&bd,&data,&source));data.pSysMem=original;CHECK_HR(T,c.device->CreateBuffer(&bd,&data,&buffer));
  bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;CHECK_HR(T,c.device->CreateBuffer(&bd,&data,&dynamic));
  D3D11_TEXTURE2D_DESC td; t.texture->GetDesc(&td);Com<ID3D11Texture2D> sourceTex;Com<ID3D11RenderTargetView> sourceView;
  CHECK_HR(T,c.device->CreateTexture2D(&td,nullptr,&sourceTex));CHECK_HR(T,c.device->CreateRenderTargetView(sourceTex,nullptr,&sourceView));
  const float red[]={1,0,0,1};c.ctx->ClearRenderTargetView(sourceView,red);
  for(bool result:{false,true})for(BOOL value:{FALSE,TRUE}){
    Com<ID3D11Predicate> p{issue(c,t,result)};bool run=result!=bool(value);
    for(unsigned op=0;op<6;op++){
      c.ctx->SetPredication(nullptr,FALSE);t.green();pred(c.ctx,p,value);
      switch(op){case 0:c.ctx->ClearRenderTargetView(t.view,red);break;
      case 1:c.ctx->CopyResource(t.texture,sourceTex);break;
      case 2:c.ctx->CopySubresourceRegion(t.texture,0,0,0,0,sourceTex,0,nullptr);break;
      case 3:ctx1->CopySubresourceRegion1(t.texture,0,0,0,0,sourceTex,0,nullptr,0);break;
      case 4:ctx1->ClearView(t.view,red,nullptr,0);break;
      case 5:{RECT r={4,4,12,12};ctx1->ClearView(t.view,red,&r,1);break;}}
      bool ok=t.isRed(run);emit("[RESOURCE] result=%u compare=%u image_op=%u correct=%u",result,value,op,ok);CHECK(T,ok);
    }
    for(unsigned op=0;op<7;op++){
      c.ctx->SetPredication(nullptr,FALSE);c.ctx->UpdateSubresource(buffer,0,nullptr,original,0,0);c.ctx->UpdateSubresource(dynamic,0,nullptr,original,0,0);
      pred(c.ctx,p,value);ID3D11Buffer* out=buffer;
      switch(op){case 0:c.ctx->CopyResource(buffer,source);break;
      case 1:c.ctx->CopySubresourceRegion(buffer,0,0,0,0,source,0,nullptr);break;
      case 2:c.ctx->UpdateSubresource(buffer,0,nullptr,changed,0,0);break;
      case 3:{D3D11_BOX b={0,0,0,4,1,1};c.ctx->UpdateSubresource(buffer,0,&b,changed,0,0);break;}
      case 4:ctx1->UpdateSubresource1(buffer,0,nullptr,changed,0,0,0);break;
      case 5:c.ctx->UpdateSubresource(dynamic,0,nullptr,changed,0,0);out=dynamic;break;
      case 6:{D3D11_BOX b={0,0,0,4,1,1};ctx1->UpdateSubresource1(dynamic,0,&b,changed,0,0,D3D11_COPY_NO_OVERWRITE);out=dynamic;break;}}
      CHECK(T,word(c,out)==(run?changed[0]:original[0]));
    }
    // Map is not predicated, including deferred staged texture uploads.
    D3D11_MAPPED_SUBRESOURCE map={};CHECK_HR(T,c.ctx->Map(dynamic,0,D3D11_MAP_WRITE_DISCARD,0,&map));memcpy(map.pData,changed,sizeof(changed));c.ctx->Unmap(dynamic,0);CHECK(T,word(c,dynamic)==changed[0]);
  }
  c.ctx->ClearState();
}
static void lists(Ctx& c) {
  const char* T="predicate_lists";c.ctx->ClearState();Target t(c);CHECK(T,t.valid,return);
  Com<ID3D11Predicate> yes{issue(c,t,true)},no{issue(c,t,false)};Com<ID3D11DeviceContext> d;CHECK_HR(T,c.device->CreateDeferredContext(0,&d));
  t.bind(d);pred(d,yes,TRUE);d->Draw(6,0);Com<ID3D11CommandList> skip;CHECK_HR(T,d->FinishCommandList(TRUE,&skip));
  Com<ID3D11Predicate> got;BOOL value;d->GetPredication(&got,&value);CHECK(T,got==yes&&value==TRUE);if(got.p){got.p->Release();got.p=nullptr;}
  // The restored deferred predicate must actually skip the next command list.
  d->Draw(6,0);Com<ID3D11CommandList> restored;CHECK_HR(T,d->FinishCommandList(FALSE,&restored));
  for(auto* list:{skip.p,restored.p}){c.ctx->SetPredication(nullptr,FALSE);t.green();c.ctx->ExecuteCommandList(list,TRUE);CHECK(T,t.isRed(false));}
  // Immediate restore TRUE restores its own predicate, not the list's value.
  c.ctx->SetPredication(nullptr,FALSE);t.green();t.bind();pred(c.ctx,no,TRUE);c.ctx->ExecuteCommandList(skip,TRUE);c.ctx->Draw(6,0);CHECK(T,t.isRed(true));
  c.ctx->SetPredication(nullptr,FALSE);t.green();t.bind();pred(c.ctx,yes,TRUE);c.ctx->ExecuteCommandList(skip,FALSE);t.bind();c.ctx->Draw(6,0);CHECK(T,t.isRed(true));
  // Query commands and predication may both be recorded in a deferred list.
  Com<ID3D11Predicate> deferredQuery{issue(c,t,true,false,d)};t.green(d);pred(d,deferredQuery,TRUE);d->Draw(6,0);Com<ID3D11CommandList> recorded;CHECK_HR(T,d->FinishCommandList(FALSE,&recorded));
  c.ctx->SetPredication(nullptr,FALSE);c.ctx->ExecuteCommandList(recorded,TRUE);CHECK(T,t.isRed(false));
  // Replaying a list must create a new immutable result generation.
  c.ctx->ExecuteCommandList(recorded,TRUE);CHECK(T,t.isRed(false));
  // Begin/End of a bound predicate are invalid and cannot reset its generation.
  pred(c.ctx,yes,TRUE);c.ctx->Begin(yes);c.ctx->End(yes);t.bind();c.ctx->Draw(6,0);CHECK(T,t.isRed(false));
  // Reuse after unbinding changes the predicate result without stale cache data.
  c.ctx->SetPredication(nullptr,FALSE);c.ctx->Begin(yes);c.ctx->End(yes);t.green();pred(c.ctx,yes,TRUE);c.ctx->Draw(6,0);CHECK(T,t.isRed(true));
  c.ctx->ClearState();
}
static void compute(Ctx& c) {
  const char* T="predicate_compute";c.ctx->ClearState();Target t(c);CHECK(T,t.valid,return);
  constexpr UINT N=256;uint32_t input[N];for(UINT i=0;i<N;i++)input[i]=i*7+3;
  D3D11_BUFFER_DESC bd={};bd.ByteWidth=sizeof(input);bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=4;
  D3D11_SUBRESOURCE_DATA data={input,0,0};Com<ID3D11Buffer> in,out,counter,args;Com<ID3D11ShaderResourceView> srv;Com<ID3D11UnorderedAccessView> uav,ctr;
  CHECK_HR(T,c.device->CreateBuffer(&bd,&data,&in));CHECK_HR(T,c.device->CreateShaderResourceView(in,nullptr,&srv));bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
  CHECK_HR(T,c.device->CreateBuffer(&bd,nullptr,&out));CHECK_HR(T,c.device->CreateUnorderedAccessView(out,nullptr,&uav));
  bd.ByteWidth=4;bd.StructureByteStride=0;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;CHECK_HR(T,c.device->CreateBuffer(&bd,nullptr,&counter));
  D3D11_UNORDERED_ACCESS_VIEW_DESC ud={};ud.Format=DXGI_FORMAT_R32_TYPELESS;ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=1;ud.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
  CHECK_HR(T,c.device->CreateUnorderedAccessView(counter,&ud,&ctr));uint32_t dispatch[]={4,1,1};bd.ByteWidth=12;bd.BindFlags=0;bd.MiscFlags=D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;data.pSysMem=dispatch;CHECK_HR(T,c.device->CreateBuffer(&bd,&data,&args));
  Com<ID3D11ComputeShader> cs;CHECK_HR(T,c.device->CreateComputeShader(dxbc_cs_main,sizeof(dxbc_cs_main),nullptr,&cs));
  for(bool result:{false,true})for(BOOL value:{FALSE,TRUE})for(bool indirect:{false,true}){
    Com<ID3D11Predicate> p{issue(c,t,result)};bool run=result!=bool(value);UINT zero[4]={};c.ctx->ClearUnorderedAccessViewUint(uav,zero);c.ctx->ClearUnorderedAccessViewUint(ctr,zero);
    c.ctx->CSSetShader(cs,nullptr,0);c.ctx->CSSetShaderResources(0,1,&srv);ID3D11UnorderedAccessView* views[]={uav,ctr};c.ctx->CSSetUnorderedAccessViews(0,2,views,nullptr);
    pred(c.ctx,p,value);if(indirect)c.ctx->DispatchIndirect(args,0);else c.ctx->Dispatch(4,1,1);
    CHECK(T,word(c,out)==(run?9u:0u));CHECK(T,word(c,counter)==(run?32640u:0u));
    // UAV clears must obey the same predicate without clearing when skipped.
    UINT seven[4]={7,7,7,7};c.ctx->ClearUnorderedAccessViewUint(uav,seven);CHECK(T,word(c,out)==(run?7u:0u));
    c.ctx->SetPredication(nullptr,FALSE);
  }
  c.ctx->ClearState();
}
static void predicate_types(Ctx& c) {
  const char* T="predicate_types";c.ctx->ClearState();
  for(auto type:{D3D11_QUERY_OCCLUSION_PREDICATE,D3D11_QUERY_SO_OVERFLOW_PREDICATE,D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0,D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1,D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2,D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3}){
    D3D11_QUERY_DESC desc={type,0};Com<ID3D11Predicate> p,qi;CHECK_HR(T,c.device->CreatePredicate(&desc,&p));
    CHECK_HR(T,p->QueryInterface(__uuidof(ID3D11Predicate),reinterpret_cast<void**>(&qi)));
    c.ctx->Begin(p);c.ctx->End(p);BOOL actual=TRUE;HRESULT hr=S_FALSE;
    for(unsigned i=0;i<10000&&hr==S_FALSE;i++){hr=c.ctx->GetData(p,&actual,sizeof(actual),0);if(hr==S_FALSE)usleep(100);}
    CHECK(T,hr==S_OK&&!actual);
  }
  D3D11_QUERY_DESC bad={D3D11_QUERY_EVENT,0};CHECK(T,c.device->CreatePredicate(&bad,nullptr)==E_INVALIDARG);
  bad={D3D11_QUERY_SO_OVERFLOW_PREDICATE,D3D11_QUERY_MISC_PREDICATEHINT};CHECK(T,c.device->CreatePredicate(&bad,nullptr)==E_INVALIDARG);
}
static void image_side_effects(Ctx& c) {
  const char* T="predicate_image";c.ctx->ClearState();Target t(c);CHECK(T,t.valid,return);
  D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=16;td.MipLevels=td.ArraySize=1;td.SampleDesc.Count=1;
  td.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
  Com<ID3D11Texture2D> depth;Com<ID3D11DepthStencilView> dsv;
  CHECK_HR(T,c.device->CreateTexture2D(&td,nullptr,&depth));CHECK_HR(T,c.device->CreateDepthStencilView(depth,nullptr,&dsv));
  td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;td.SampleDesc.Count=4;
  Com<ID3D11Texture2D> ms;Com<ID3D11RenderTargetView> msview;CHECK_HR(T,c.device->CreateTexture2D(&td,nullptr,&ms));CHECK_HR(T,c.device->CreateRenderTargetView(ms,nullptr,&msview));
  td.SampleDesc.Count=1;td.MipLevels=3;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;td.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;
  Com<ID3D11Texture2D> mip;Com<ID3D11ShaderResourceView> mipview;CHECK_HR(T,c.device->CreateTexture2D(&td,nullptr,&mip));CHECK_HR(T,c.device->CreateShaderResourceView(mip,nullptr,&mipview));
  td.MipLevels=1;td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;td.MiscFlags=0;
  Com<ID3D11Texture2D> ua;Com<ID3D11UnorderedAccessView> uaView;CHECK_HR(T,c.device->CreateTexture2D(&td,nullptr,&ua));CHECK_HR(T,c.device->CreateUnorderedAccessView(ua,nullptr,&uaView));
  D3D11_BUFFER_DESC bd={};bd.ByteWidth=64;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;Com<ID3D11Buffer> floatBuffer,counterBuffer,copyCount;
  CHECK_HR(T,c.device->CreateBuffer(&bd,nullptr,&floatBuffer));D3D11_UNORDERED_ACCESS_VIEW_DESC ud={};ud.Format=DXGI_FORMAT_R32_FLOAT;ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=16;
  Com<ID3D11UnorderedAccessView> floatView,counterView;CHECK_HR(T,c.device->CreateUnorderedAccessView(floatBuffer,&ud,&floatView));
  bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=4;CHECK_HR(T,c.device->CreateBuffer(&bd,nullptr,&counterBuffer));ud.Format=DXGI_FORMAT_UNKNOWN;ud.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_COUNTER;CHECK_HR(T,c.device->CreateUnorderedAccessView(counterBuffer,&ud,&counterView));
  bd.BindFlags=bd.MiscFlags=bd.StructureByteStride=0;CHECK_HR(T,c.device->CreateBuffer(&bd,nullptr,&copyCount));
  const float red[]={1,0,0,1},green[]={0,1,0,1},ones[]={1,1,1,1};uint32_t redPixels[256],greenPixels[256],zero[16]={};
  std::fill_n(redPixels,256,0xff0000ffu);std::fill_n(greenPixels,256,0xff00ff00u);
  for(bool result:{false,true})for(BOOL value:{FALSE,TRUE}){
    Com<ID3D11Predicate> p{issue(c,t,result)};bool run=result!=bool(value);
    c.ctx->SetPredication(nullptr,FALSE);t.green();c.ctx->ClearRenderTargetView(msview,red);
    pred(c.ctx,p,value);c.ctx->ResolveSubresource(t.texture,0,ms,0,DXGI_FORMAT_R8G8B8A8_UNORM);CHECK(T,t.isRed(run));
    c.ctx->SetPredication(nullptr,FALSE);t.green();pred(c.ctx,p,value);c.ctx->UpdateSubresource(t.texture,0,nullptr,redPixels,64,1024);CHECK(T,t.isRed(run));
    c.ctx->SetPredication(nullptr,FALSE);c.ctx->ClearDepthStencilView(dsv,3,0.25f,0x55);pred(c.ctx,p,value);c.ctx->ClearDepthStencilView(dsv,3,0.75f,0xaa);
    c.ctx->SetPredication(nullptr,FALSE);auto depthBytes=readback_tex2d(c,depth,0,4);uint32_t d=0;CHECK(T,depthBytes.size()==1024,return);memcpy(&d,depthBytes.data(),4);CHECK(T,(d>>24)==(run?0xaau:0x55u));CHECK(T,std::abs(double(d&0xffffff)/16777215.0-(run?.75:.25))<.00001);
    for(UINT m=0;m<3;m++)c.ctx->UpdateSubresource(mip,m,nullptr,m?greenPixels:redPixels,(16>>m)*4,0);
    pred(c.ctx,p,value);c.ctx->GenerateMips(mipview);c.ctx->SetPredication(nullptr,FALSE);auto small=readback_tex2d(c,mip,2,4);CHECK(T,small.size()==64,return);CHECK(T,px_eq(small,4,1,1,run?255:0,run?0:255,0,255,0));
    c.ctx->ClearUnorderedAccessViewFloat(uaView,green);pred(c.ctx,p,value);c.ctx->ClearUnorderedAccessViewFloat(uaView,red);c.ctx->SetPredication(nullptr,FALSE);auto u=readback_tex2d(c,ua,0,4);CHECK(T,u.size()==1024,return);CHECK(T,px_eq(u,16,8,8,run?255:0,run?0:255,0,255,0));
    c.ctx->ClearUnorderedAccessViewFloat(uaView,green);pred(c.ctx,p,value);UINT rgba[]={255,0,0,255};c.ctx->ClearUnorderedAccessViewUint(uaView,rgba);c.ctx->SetPredication(nullptr,FALSE);u=readback_tex2d(c,ua,0,4);CHECK(T,u.size()==1024,return);CHECK(T,px_eq(u,16,8,8,run?255:0,run?0:255,0,255,0));
    c.ctx->ClearUnorderedAccessViewFloat(floatView,green);pred(c.ctx,p,value);c.ctx->ClearUnorderedAccessViewFloat(floatView,ones);CHECK(T,word(c,floatBuffer)==(run?0x3f800000u:0u));
    c.ctx->SetPredication(nullptr,FALSE);c.ctx->UpdateSubresource(copyCount,0,nullptr,zero,0,0);pred(c.ctx,p,value);UINT initial=13;c.ctx->CSSetUnorderedAccessViews(0,1,&counterView,&initial);c.ctx->CopyStructureCount(copyCount,0,counterView);CHECK(T,word(c,copyCount)==(run?13u:0u));
    // Binding counter reset remains unconditional even when CopyStructureCount skips.
    c.ctx->SetPredication(nullptr,FALSE);c.ctx->CopyStructureCount(copyCount,0,counterView);CHECK(T,word(c,copyCount)==13u);
  }
  c.ctx->ClearState();
}

static void stream_predicates(Ctx& c) {
  const char* T="predicate_stream_output";c.ctx->ClearState();Target t(c);CHECK(T,t.valid,return);
  D3D11_SO_DECLARATION_ENTRY entries[]={{0,"SV_Position",0,0,2,0},{0,"TEXCOORD",0,0,2,0},{0,"COLOR",0,0,4,0}};
  UINT stride=sizeof(Vertex);Com<ID3D11GeometryShader> so;
  CHECK_HR(T,c.device->CreateGeometryShaderWithStreamOutput(dxbc_vs_pass,sizeof(dxbc_vs_pass),entries,3,&stride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&so));
  for(auto type:{D3D11_QUERY_SO_OVERFLOW_PREDICATE,D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0})for(bool overflow:{false,true}) {
    D3D11_BUFFER_DESC bd={};bd.ByteWidth=(overflow?3:6)*sizeof(Vertex);bd.BindFlags=D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_VERTEX_BUFFER;
    Com<ID3D11Buffer> output;CHECK_HR(T,c.device->CreateBuffer(&bd,nullptr,&output));
    D3D11_QUERY_DESC qd={type,0};Com<ID3D11Predicate> p;CHECK_HR(T,c.device->CreatePredicate(&qd,&p));
    c.ctx->SetPredication(nullptr,FALSE);t.bind();c.ctx->GSSetShader(so,nullptr,0);UINT zero=0;c.ctx->SOSetTargets(1,&output,&zero);
    c.ctx->Begin(p);c.ctx->Draw(6,0);c.ctx->End(p);c.ctx->SOSetTargets(0,nullptr,nullptr);c.ctx->GSSetShader(nullptr,nullptr,0);
    for(BOOL value:{FALSE,TRUE}){c.ctx->SetPredication(nullptr,FALSE);t.green();pred(c.ctx,p,value);t.draw(0);CHECK(T,t.isRed(overflow!=bool(value)));}
    c.ctx->SetPredication(nullptr,FALSE);BOOL actual=!overflow;HRESULT hr=S_FALSE;for(unsigned i=0;i<10000&&hr==S_FALSE;i++){hr=c.ctx->GetData(p,&actual,sizeof(actual),0);if(hr==S_FALSE)usleep(100);}
    CHECK(T,hr==S_OK&&!!actual==overflow);
    if(!overflow){Com<ID3D11Predicate> yes{issue(c,t,true)};
      for(BOOL value:{FALSE,TRUE}){c.ctx->SetPredication(nullptr,FALSE);t.green();t.bind();c.ctx->IASetVertexBuffers(0,1,&output,&stride,&zero);pred(c.ctx,yes,value);c.ctx->DrawAuto();CHECK(T,t.isRed(!bool(value)));}
    }
  }
  c.ctx->ClearState();
}

// R8/R16 IA offsets may be unaligned for Vulkan's byte-count bias. Capture the
// fetched values as well as the count: normalizing the IA binding would be wrong.
static void draw_auto_offsets(Ctx& c) {
  const char* T="draw_auto_offsets";
  c.ctx->ClearState();
  Target t(c);
  CHECK(T,t.valid,return);
  D3D11_SO_DECLARATION_ENTRY entries[]={{0,"SV_Position",0,0,2,0},
    {0,"TEXCOORD",0,0,2,0},{0,"COLOR",0,0,4,0}};
  UINT stride=sizeof(Vertex),zero=0;
  Com<ID3D11GeometryShader> so;
  CHECK_HR(T,c.device->CreateGeometryShaderWithStreamOutput(dxbc_vs_pass,
    sizeof(dxbc_vs_pass),entries,3,&stride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&so));
  uint8_t initial[8*sizeof(Vertex)]={};
  D3D11_BUFFER_DESC bd={};
  bd.ByteWidth=sizeof(initial);
  bd.BindFlags=D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA data={initial,0,0};
  Com<ID3D11Buffer> source,capture;
  CHECK_HR(T,c.device->CreateBuffer(&bd,&data,&source));
  CHECK_HR(T,c.device->CreateBuffer(&bd,&data,&capture));
  t.bind();
  c.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  c.ctx->GSSetShader(so,nullptr,0);
  c.ctx->SOSetTargets(1,&source,&zero);
  c.ctx->Draw(6,0);
  c.ctx->SOSetTargets(0,nullptr,nullptr);
  auto sourceBytes=readback_buffer(c,source);
  CHECK(T,sourceBytes.size()==sizeof(initial),return);
  D3D11_QUERY_DESC qd={D3D11_QUERY_PIPELINE_STATISTICS,0};
  Com<ID3D11Query> statistics;
  CHECK_HR(T,c.device->CreateQuery(&qd,&statistics));
  for(unsigned width:{1u,2u}) {
    const DXGI_FORMAT format=width==1 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM;
    D3D11_INPUT_ELEMENT_DESC elements[]={{"POSITION",0,format,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
      {"TEXCOORD",0,format,0,8,D3D11_INPUT_PER_VERTEX_DATA,0},
      {"COLOR",0,format,0,16,D3D11_INPUT_PER_VERTEX_DATA,0}};
    Com<ID3D11InputLayout> layout;
    CHECK_HR(T,c.device->CreateInputLayout(elements,3,dxbc_vs_pass,sizeof(dxbc_vs_pass),&layout));
    c.ctx->IASetInputLayout(layout);
    for(UINT offset:{0u,1u,2u,3u,4u,31u,32u,33u,158u,159u,160u,161u,190u,191u,192u,193u,196u,0u}) {
      if(offset%width) continue;
      c.ctx->IASetVertexBuffers(0,1,&source,&stride,&offset);
      c.ctx->SOSetTargets(1,&capture,&zero);
      c.ctx->Begin(statistics);
      c.ctx->DrawAuto();
      c.ctx->End(statistics);
      c.ctx->SOSetTargets(0,nullptr,nullptr);
      D3D11_QUERY_DATA_PIPELINE_STATISTICS observed={};
      HRESULT hr=S_FALSE;
      for(unsigned i=0;i<10000&&hr==S_FALSE;i++) {
        hr=c.ctx->GetData(statistics,&observed,sizeof(observed),0);
        if(hr==S_FALSE) usleep(100);
      }
      CHECK(T,hr==S_OK,return);
      const UINT filled=6*sizeof(Vertex);
      const UINT count=offset>=filled ? 0 : (filled-offset)/sizeof(Vertex);
      emit("[DRAWAUTO-OFFSET] format=R%u offset=%u expected=%u observed=%llu",
        width*8,offset,count,static_cast<unsigned long long>(observed.IAVertices));
      CHECK(T,observed.IAVertices==count);
      CHECK(T,observed.CSInvocations==0);
      Com<ID3D11Buffer> bound;
      UINT actualStride=0,actualOffset=0;
      c.ctx->IAGetVertexBuffers(0,1,&bound,&actualStride,&actualOffset);
      CHECK(T,bound==source&&actualStride==stride&&actualOffset==offset);
      auto bytes=readback_buffer(c,capture);
      CHECK(T,bytes.size()==sizeof(initial),return);
      for(UINT vertex=0;vertex<count;vertex++) {
        float expected[8]={0,0,0,0,0,0,0,1};
        for(unsigned element:{0u,2u,4u}) {
          const auto* input=sourceBytes.data()+offset+vertex*stride+element*sizeof(float);
          uint16_t raw=*input;
          if(width==2) memcpy(&raw,input,sizeof(raw));
          expected[element]=float(raw)/(width==1 ? 255.0f : 65535.0f);
        }
        float actual[8];memcpy(actual,bytes.data()+vertex*stride,sizeof(actual));
        bool matches=true;
        for(unsigned component=0;component<8;component++)
          matches=matches&&std::abs(actual[component]-expected[component])<0.000001f;
        CHECK(T,matches);
      }
    }
  }
  // Normalization must leave the original counter available for SO append.
  t.bind();
  c.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  c.ctx->GSSetShader(so,nullptr,0);
  UINT append=~0u;
  c.ctx->SOSetTargets(1,&source,&append);
  c.ctx->Draw(2,0);
  c.ctx->SOSetTargets(0,nullptr,nullptr);
  auto countDraw=[&](ID3D11DeviceContext* ctx) {
    UINT offset=4;
    t.bind(ctx);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ctx->GSSetShader(so,nullptr,0);
    ctx->IASetVertexBuffers(0,1,&source,&stride,&offset);
    ctx->SOSetTargets(1,&capture,&zero);
    ctx->Begin(statistics);
    ctx->DrawAuto();
    ctx->End(statistics);
    ctx->SOSetTargets(0,nullptr,nullptr);
  };
  auto checkCount=[&](UINT expected) {
    D3D11_QUERY_DATA_PIPELINE_STATISTICS observed={};
    HRESULT hr=S_FALSE;
    for(unsigned i=0;i<10000&&hr==S_FALSE;i++) {
      hr=c.ctx->GetData(statistics,&observed,sizeof(observed),0);
      if(hr==S_FALSE) usleep(100);
    }
    CHECK(T,hr==S_OK);
    CHECK(T,observed.IAVertices==expected);
    CHECK(T,observed.CSInvocations==0);
  };
  countDraw(c.ctx);
  checkCount(7);
  // Replaying a deferred list must get its own normalized counter storage.
  Com<ID3D11DeviceContext> deferred;
  Com<ID3D11CommandList> list;
  CHECK_HR(T,c.device->CreateDeferredContext(0,&deferred));
  countDraw(deferred);
  CHECK_HR(T,deferred->FinishCommandList(FALSE,&list));
  for(unsigned repeat=0;repeat<2;repeat++) {
    c.ctx->ExecuteCommandList(list,FALSE);
    checkCount(7);
  }
  c.ctx->GSSetShader(nullptr,nullptr,0);
  Com<ID3D11Predicate> yes{issue(c,t,true)};
  for(BOOL value:{FALSE,TRUE}) {
    pred(c.ctx,yes,value);
    countDraw(c.ctx);
    c.ctx->SetPredication(nullptr,FALSE);
    checkCount(value ? 0 : 7);
  }
  // A normal draw after both beyond-end offsets proves the queue is still usable.
  CHECK(T,c.device->GetDeviceRemovedReason()==S_OK);
  c.ctx->ClearState();
}

static void state_edges(Ctx& c) {
  const char* T="predicate_state_edges";c.ctx->ClearState();Target t(c);CHECK(T,t.valid,return);
  Com<ID3D11Predicate> yes{issue(c,t,true)};D3D11_QUERY_DESC qd={D3D11_QUERY_OCCLUSION_PREDICATE,0};Com<ID3D11Predicate> unissued;
  CHECK_HR(T,c.device->CreatePredicate(&qd,&unissued));pred(c.ctx,yes,TRUE);c.ctx->SetPredication(unissued,TRUE);
  Com<ID3D11Predicate> got;BOOL compare;c.ctx->GetPredication(&got,&compare);CHECK(T,got==yes&&compare==TRUE);
  if(got.p){got.p->Release();got.p=nullptr;}
  c.ctx->SetPredication(nullptr,FALSE);t.green();pred(c.ctx,yes,17);c.ctx->Draw(6,0);CHECK(T,t.isRed(false));
  c.ctx->ClearState();t.bind();c.ctx->Draw(6,0);CHECK(T,t.isRed(true));
  Com<ID3D11Device1> device1;Com<ID3D11DeviceContext1> ctx1;
  CHECK_HR(T,c.device->QueryInterface(__uuidof(ID3D11Device1),reinterpret_cast<void**>(&device1)));
  CHECK_HR(T,c.ctx->QueryInterface(__uuidof(ID3D11DeviceContext1),reinterpret_cast<void**>(&ctx1)));
  Com<ID3DDeviceContextState> fresh,saved,old;D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0,selected;
  CHECK_HR(T,device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&selected,&fresh));
  t.green();pred(c.ctx,yes,TRUE);ctx1->SwapDeviceContextState(fresh,&saved);t.bind();c.ctx->Draw(6,0);CHECK(T,t.isRed(true));
  t.green();ctx1->SwapDeviceContextState(saved,&old);c.ctx->Draw(6,0);CHECK(T,t.isRed(false));
  // Releasing external state/query references must retain the active result.
  saved.p->Release();saved.p=nullptr;
  yes.p->Release();yes.p=nullptr;c.ctx->Draw(6,0);CHECK(T,t.isRed(false));
  // Query hint changes scheduling permission, not the equality comparison.
  c.ctx->SetPredication(nullptr,FALSE);qd.MiscFlags=D3D11_QUERY_MISC_PREDICATEHINT;Com<ID3D11Predicate> hint;
  CHECK_HR(T,c.device->CreatePredicate(&qd,&hint));c.ctx->Begin(hint);c.ctx->Draw(6,0);c.ctx->End(hint);t.green();pred(c.ctx,hint,TRUE);c.ctx->Draw(6,0);CHECK(T,t.isRed(false));
  // Deferred Map uploads are unconditional even when a draw would be skipped.
  D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=4;td.MipLevels=td.ArraySize=1;td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.Usage=D3D11_USAGE_DYNAMIC;td.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
  Com<ID3D11Texture2D> dynamic;CHECK_HR(T,c.device->CreateTexture2D(&td,nullptr,&dynamic));Com<ID3D11DeviceContext> def;CHECK_HR(T,c.device->CreateDeferredContext(0,&def));
  pred(def,hint,TRUE);D3D11_MAPPED_SUBRESOURCE map={};CHECK_HR(T,def->Map(dynamic,0,D3D11_MAP_WRITE_DISCARD,0,&map));
  for(UINT y=0;y<4;y++)for(UINT x=0;x<4;x++)static_cast<uint32_t*>(static_cast<void*>(static_cast<char*>(map.pData)+y*map.RowPitch))[x]=0xff030201;
  def->Unmap(dynamic,0);Com<ID3D11CommandList> list;CHECK_HR(T,def->FinishCommandList(FALSE,&list));c.ctx->ExecuteCommandList(list,TRUE);
  c.ctx->SetPredication(nullptr,FALSE);auto image=readback_tex2d(c,dynamic,0,4);CHECK(T,image.size()==64,return);CHECK(T,px_eq(image,4,2,2,1,2,3,255,0));
  c.ctx->ClearState();
}

int main(int argc,char** argv){g_out=stdout;dropPredicate=argc>1&&!strcmp(argv[1],"--drop-predicate");Ctx c;
  if(create_device(c)){draws(c);resources(c);lists(c);compute(c);predicate_types(c);image_side_effects(c);stream_predicates(c);draw_auto_offsets(c);state_edges(c);}
  emit("[PREDICATION-SUMMARY] passed=%d failed=%d",g_passes,g_fails);return g_fails?1:0;}
