#!/usr/bin/env python3
"""GPU-free DrawAuto regression: execute production methods against a Vulkan spy.

This proves count arithmetic and emitted call constraints, not GPU execution.
The native R8/R16 fetch/readback oracle lives in test-predication-backend.cpp.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(path, marker):
    text = (ROOT / path).read_text()
    start = text.index(marker)
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


backend = function('triton-dxvk/src/dxvk/dxvk_context.cpp',
                   'void DxvkContext::drawIndirectXfb(')
shader = function('triton-dxvk/src/dxvk/shaders/dxvk_draw_auto.comp', 'void main()').replace('void main()', 'void normalizeShader()')
frontend = function('triton-dxvk/src/d3d11/d3d11_context.cpp',
                    'void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DrawAuto()')
wrapper = function('triton-dxvk/src/dxvk/dxvk_cmdlist.h',
                   'void cmdDrawIndirectVertexCount(')
header = (ROOT / 'triton-dxvk/src/dxvk/dxvk_context.h').read_text()
start = header.index('void drawIndirectXfb(')
declaration = header[start:header.index(';', start) + 1]

PREAMBLE = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>
using UINT = uint32_t;
using VkDeviceSize = uint64_t;
using VkBuffer = uint64_t;
using VkCommandBuffer = uint64_t;
#define STDMETHODCALLTYPE
#define unlikely(x) (x)
struct VkConditionalRenderingBeginInfoEXT { VkBuffer buffer = 0; };
struct { struct { uint32_t value=0; } source, destination; uint32_t bias=0; } args;
void normalizeShader();
static uint64_t checks = 0;
static void require(bool ok, const char* why) {
  ++checks;
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
struct Vulkan {
  uint32_t calls=0, bias=0, stride=0, instances=0, first=0, begins=0, ends=0;
  VkDeviceSize offset=0;
  VkBuffer buffer=0;
  void vkCmdBeginConditionalRenderingEXT(VkCommandBuffer, const VkConditionalRenderingBeginInfoEXT*) { ++begins; }
  void vkCmdEndConditionalRenderingEXT(VkCommandBuffer) { ++ends; }
  void vkCmdDrawIndirectByteCountEXT(VkCommandBuffer, uint32_t count, uint32_t firstInstance,
      VkBuffer counterBuffer, VkDeviceSize counterBufferOffset, uint32_t counterOffset, uint32_t vertexStride) {
    ++calls; bias=counterOffset; stride=vertexStride; instances=count; first=firstInstance;
    offset=counterBufferOffset; buffer=counterBuffer;
    require((counterOffset & 3u) == 0, "VUID-counterOffset-09474");
    require(vertexStride && !(vertexStride & 3u), "VUID-vertexStride-09475/02289");
    require(!(counterBufferOffset & 3u), "Vulkan counter-buffer address alignment");
  }
};
enum class DxvkStatCounter { CmdDrawCalls };
enum class DxvkContextFlag { GpRenderPassUnsynchronized };
struct Buffer {
  uint32_t stride=32, bytes=0;
  bool stores=false;
  bool hasGfxStores() const { return stores; }
  uint32_t getXfbVertexStride() const { return stride; }
};
struct DxvkBufferSlice {
  Buffer* resource=nullptr;
  VkDeviceSize base=0;
  bool defined() const { return resource; }
  Buffer* buffer() const { return resource; }
  VkDeviceSize offset() const { return base; }
  struct Info { VkBuffer buffer; VkDeviceSize offset; };
  Info getSliceInfo() const { return {resource ? 123u : 0u, base}; }
};
struct CommandList {
  Vulkan* m_vkd;
  uint32_t stats=0;
  VkCommandBuffer getCmdBuffer() { return 7; }
  void addStatCtr(DxvkStatCounter, uint32_t count) { stats+=count; }
'''

CONTEXT = r'''
};
struct DxvkContext {
  explicit DxvkContext(CommandList* cmd): m_cmd(cmd) {}
  CommandList* m_cmd;
  struct { struct { DxvkBufferSlice cntBuffer; } id; } m_state{};
  struct { bool value=false; bool test(DxvkContextFlag) const { return value; } } m_flags;
  bool predicateAllowed=true, gpuPredicate=false, pipelineAllowed=true;
  uint32_t accesses=0, bindings=0, normalizations=0;
  VkDeviceSize normalizedOffset=0;
  VkDeviceSize accessedOffset=0;
  bool preparePredicate(VkConditionalRenderingBeginInfoEXT& info) { info.buffer=gpuPredicate ? 456u : 0u; return predicateAllowed; }
  template<bool, bool> bool commitGraphicsState() { return pipelineAllowed; }
  DxvkBufferSlice::Info normalizeXfbCounter(VkDeviceSize offset, uint32_t bias) {
    ++normalizations; normalizedOffset=offset;
    args.source.value=m_state.id.cntBuffer.buffer()->bytes; args.bias=bias;
    normalizeShader(); return {321u,128u};
  }
  void accessDrawCountBuffer(VkDeviceSize offset) { ++accesses; accessedOffset=offset; }
  void bindDrawBuffers(DxvkBufferSlice, DxvkBufferSlice counter) { ++bindings; m_state.id.cntBuffer=counter; }
'''

FRONTEND = r'''
};
struct D3D11Buffer {
  DxvkBufferSlice vertices, counter;
  DxvkBufferSlice GetBufferSlice() const { return vertices; }
  DxvkBufferSlice GetSOCounter() const { return counter; }
};
struct D3D10DeviceLock { ~D3D10DeviceLock() {} };
struct Forwarder { template<class T> static T&& move(T& value) { return std::move(value); } };
template<class ContextType> struct D3D11CommonContext {
  explicit D3D11CommonContext(DxvkContext* ctx): context(ctx) {}
  DxvkContext* context;
  struct State {
    struct { struct {
      struct { D3D11Buffer* value=nullptr; D3D11Buffer* ptr() const { return value; } } buffer;
      UINT offset=0;
    } vertexBuffers[1]; } ia;
    struct { unsigned resets=0; void reset() { ++resets; } } id;
  } m_state{};
  bool dirty=false;
  unsigned dirtyApplied=0;
  D3D10DeviceLock LockContext() { return {}; }
  bool HasDirtyGraphicsBindings() const { return dirty; }
  void ApplyDirtyGraphicsBindings() { ++dirtyApplied; }
  template<class F> void EmitCs(F f) { f(context); }
  void DrawAuto();
};
'''

TESTS = r'''
static uint64_t reference(uint32_t bytes, VkDeviceSize bias, uint32_t stride) {
  // Independent D3D interval length; no alignment/rounding in this oracle.
  return bias >= bytes ? 0 : (uint64_t(bytes) - bias) / stride;
}
static void checkCount(uint32_t bytes, uint32_t stride, VkDeviceSize bias) {
  Vulkan vk; CommandList cmd{&vk}; DxvkContext ctx{&cmd}; Buffer buffer;
  buffer.bytes=bytes; ctx.m_state.id.cntBuffer={&buffer,32}; ctx.m_flags.value=true;
  ctx.drawIndirectXfb(16,stride,bias);
  const uint64_t expected=reference(bytes,bias,stride);
  const uint32_t gpuBytes=ctx.normalizations ? args.destination.value : bytes;
  // Model an unsaturated implementation to make driver-dependent subtraction fail.
  const uint64_t actual=vk.calls ? uint32_t(gpuBytes-vk.bias)/vk.stride : 0;
  require(actual==expected, "exact DrawAuto vertex count");
  require(vk.calls==(bias<=0xfffffffcu), "bounded bias must issue exactly one call or none");
  if(vk.calls) {
    require(vk.instances==1 && vk.first==0 && vk.buffer==(bias ? 321u : 123u) && vk.offset==(bias ? 128u : 48u),
      "actual Vulkan call retains count buffer, addressing and instance parameters");
    require(vk.stride==stride, "stream-output divisor is unchanged");
    require(vk.bias==0, "Vulkan subtraction never underflows or needs bias alignment");
    require(cmd.stats==1 && ctx.accesses==!bias && ctx.normalizations==!!bias,
      "statistics and source counter access preserved");
    require((bias ? ctx.normalizedOffset : ctx.accessedOffset)==16,
      "normalization reads the requested source counter");
    require(buffer.bytes==bytes, "normalization preserves the original SO counter");
  }
}
static void checkFrontend(VkDeviceSize backing, uint32_t iaOffset, uint32_t stride) {
  Vulkan vk; CommandList cmd{&vk}; DxvkContext ctx{&cmd}; Buffer buffer;buffer.stride=stride;
  D3D11Buffer resource{{&buffer,backing},{&buffer,64}};
  D3D11CommonContext<int> frontend{&ctx}; frontend.dirty=true;
  frontend.m_state.ia.vertexBuffers[0].buffer.value=&resource;
  frontend.m_state.ia.vertexBuffers[0].offset=iaOffset;
  frontend.DrawAuto();
  const auto bias=backing+iaOffset;
  require(frontend.m_state.ia.vertexBuffers[0].offset==iaOffset,
    "actual IA fetch offset remains unchanged");
  require(resource.vertices.offset()==backing, "actual backing slice remains unchanged");
  require(vk.calls==(bias<=0xfffffffcu), "full buffer+IA offset reaches range check before narrowing");
  if(vk.calls) {
    require(vk.bias==0 && (!bias || args.bias==bias), "frontend count bias");
    require(vk.stride==stride && vk.offset==(bias ? 128u : 64u), "SO stride and counter-buffer location");
  }
  require(frontend.dirtyApplied==1 && frontend.m_state.id.resets==1,
    "frontend dirty state and indirect tracking preserved");
  require(ctx.bindings==2 && !ctx.m_state.id.cntBuffer.defined(),
    "counter binding released even for empty draw");
}
int main() {
  // Exhaust every residue and count/stride boundary in this bounded domain.
  for(uint32_t stride=4;stride<=128;stride+=4)
    for(uint32_t bytes=0;bytes<=1024;bytes+=4)
      for(uint32_t bias=0;bias<=1031;++bias) checkCount(bytes,stride,bias);
  // All valid D3D SO stride values, every offset around selected count boundaries.
  for(uint32_t stride=4;stride<=2048;stride+=4)
    for(uint32_t bytes:{0u,4u,2048u,0x7ffffffcu,0x80000000u,0xfffff800u,0xfffffffcu})
      for(int delta=-8;delta<=8;++delta) {
        int64_t boundary=int64_t(bytes)+delta;
        if(boundary>=0) checkCount(bytes,stride,VkDeviceSize(boundary));
        checkCount(bytes,stride,VkDeviceSize(0xfffffff8ull+delta+8));
      }
  // Floor-division transitions at high counts, including partial strides.
  for(uint32_t stride=4;stride<=2048;stride+=4)
    for(uint32_t bytes:{2048u,0x7ffffffcu,0x80000000u,0xfffffffcu})
      for(unsigned vertices:{1u,2u,3u,1024u})
        for(int delta=-4;delta<=4;++delta) {
          const int64_t bias=int64_t(bytes)-int64_t(stride)*vertices+delta;
          if(bias>=0) checkCount(bytes,stride,VkDeviceSize(bias));
        }
  uint64_t rng=0x26d3d11;
  for(unsigned i=0;i<200000;++i) {
    auto next=[&]() { rng^=rng<<13; rng^=rng>>7; rng^=rng<<17; return uint32_t(rng); };
    uint32_t bytes=next()&~3u, stride=(next()%512+1)*4, bias=next();
    checkCount(bytes,stride,bias);
  }
  for(VkDeviceSize backing:{0ull,256ull,4096ull,0xfffffff0ull,0x100000000ull,0x10000000000ull})
    for(uint32_t offset:{0u,1u,2u,3u,4u,31u,32u,33u,0xfffffffcu,0xfffffffdu,0xfffffffeu,0xffffffffu})
      for(uint32_t stride:{4u,32u,2048u}) checkFrontend(backing,offset,stride);
  // Conditional execution, failed pipeline admission and counter-write tracking.
  Vulkan vk; CommandList cmd{&vk}; DxvkContext ctx{&cmd}; Buffer buffer;
  ctx.m_state.id.cntBuffer={&buffer,0};ctx.predicateAllowed=false;
  ctx.drawIndirectXfb(0,32,1);require(!vk.calls,"CPU predicate still suppresses draw");
  ctx.predicateAllowed=true;ctx.pipelineAllowed=false;
  ctx.drawIndirectXfb(0,32,1);require(!vk.calls,"failed pipeline still suppresses draw");
  ctx.pipelineAllowed=true;ctx.gpuPredicate=true;buffer.stores=true;
  ctx.drawIndirectXfb(0,32,1);require(vk.calls==1&&vk.begins==1&&vk.ends==1,
    "conditional Vulkan block preserved");
  require(ctx.normalizations==2,"counter GPU normalization preserved");
  D3D11CommonContext<int> empty{&ctx};empty.DrawAuto();
  require(ctx.bindings==0,"unbound slot zero emits no commands");
  D3D11Buffer resource{{&buffer,0},{nullptr,0}};
  empty.m_state.ia.vertexBuffers[0].buffer.value=&resource;empty.DrawAuto();
  require(ctx.bindings==0,"missing SO counter emits no commands");
  std::printf("DrawAuto production oracle passed: %llu checks\n",(unsigned long long)checks);
}
'''


def source(body, api=declaration, front=frontend):
    return (PREAMBLE + wrapper + CONTEXT + api + FRONTEND + shader + body + '\n'
            + 'template<typename ContextType>\n' + front + TESTS)


with tempfile.TemporaryDirectory(prefix='triton-draw-auto-') as temporary:
    directory = Path(temporary)

    def run(name, text, positive):
        cpp = directory / (name + '.cpp')
        exe = directory / name
        cpp.write_text(text)
        subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++17', '-O2',
                        '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], text=True, capture_output=True, timeout=60)
        if positive:
            assert result.returncode == 0, result.stdout + result.stderr
            assert 'DrawAuto production oracle passed:' in result.stdout
            print(result.stdout.strip())
        else:
            assert result.returncode != 0 and 'FAIL:' in result.stderr, name
            print(name + ' rejected: ' + result.stderr.strip())

    run('production', source(backend), True)
    # Fault injection modifies the actual extracted method, never its oracle.
    bias = 'const uint32_t drawCounterBias = uint32_t(counterBias);'
    assert bias in backend
    run('negative-raw-vulkan-bias', source(backend.replace(
        '0, counterDivisor, predicate);', 'drawCounterBias, counterDivisor, predicate);')), False)
    run('negative-round-down', source(backend.replace(bias,
        'const uint32_t drawCounterBias = uint32_t(counterBias) & ~3u;')), False)
    clamped = 'counter > args.bias ? counter - args.bias : 0u'
    assert clamped in shader
    run('negative-counter-underflow', source(backend).replace(clamped, 'counter - args.bias'), False)
    guard = 'if (counterBias > uint64_t(0xfffffffc))\n      return;'
    assert guard in backend
    run('negative-wrap-overflow', source(backend.replace(guard, '')), False)
    run('negative-premature-narrowing', source(
        backend.replace('VkDeviceSize      counterBias', 'uint32_t          counterBias'),
        declaration.replace('VkDeviceSize      counterBias', 'uint32_t          counterBias')), False)
    binding = 'const UINT vertexOffset = m_state.ia.vertexBuffers[0].offset;'
    assert binding in frontend
    run('negative-normalize-ia-binding', source(backend, front=frontend.replace(binding,
        'const UINT vertexOffset = m_state.ia.vertexBuffers[0].offset = '
        '(m_state.ia.vertexBuffers[0].offset + 3u) & ~3u;')), False)
print('DrawAuto CPU and Vulkan-parameter checks passed')
