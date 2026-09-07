// Shaders for the native offscreen D3D11 smoke suite.  Compiled offline with
// fxc (SM 5.0); the DXBC is embedded into the test binary, so no d3dcompiler
// is needed at run time (there is none on iOS).

struct VSIn {
  float2 pos : POSITION;
  float2 uv : TEXCOORD0;
  float4 color : COLOR0;
};

struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
  float4 color : COLOR0;
};

VSOut vs_pass(VSIn input) {
  VSOut o;
  o.pos = float4(input.pos, 0.5, 1.0);
  o.uv = input.uv;
  o.color = input.color;
  return o;
}

VSOut vs_inst(VSIn input, uint iid : SV_InstanceID) {
  VSOut o;
  o.pos = float4(input.pos + float2(0.5 * iid, 0.0), 0.5, 1.0);
  o.uv = input.uv;
  o.color = input.color * (1.0 / (1u << iid));
  return o;
}

// vs_depth: z comes packed in uv.x so the same input layout works.
VSOut vs_depth(VSIn input) {
  VSOut o;
  o.pos = float4(input.pos, input.uv.x, 1.0);
  o.uv = input.uv;
  o.color = input.color;
  return o;
}

float4 ps_color(VSOut input) : SV_Target {
  return input.color;
}

cbuffer Constants : register(b0) {
  float4 cb_color;
};

float4 ps_cbuf(VSOut input) : SV_Target {
  return cb_color;
}

Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);

float4 ps_tex(VSOut input) : SV_Target {
  return tex0.Sample(samp0, input.uv);
}

StructuredBuffer<uint> cs_in : register(t0);
RWStructuredBuffer<uint> cs_out : register(u0);
RWByteAddressBuffer cs_counter : register(u1);

[numthreads(64, 1, 1)]
void cs_main(uint3 dtid : SV_DispatchThreadID) {
  cs_out[dtid.x] = cs_in[dtid.x] * 3u + dtid.x;
  cs_counter.InterlockedAdd(0, dtid.x);
}

// ps_compat: DXBC patterns whose airconv translation used to emit constructs
// pre-LLVM-8-era AIR bitcode readers reject (fneg/UNOP from negation, funnel
// shift from the rotate idiom, min/max intrinsics from integer ternaries).
// Values are exact so the test can pixel-check the lowered expansions.
float4 ps_compat(VSOut input) : SV_Target {
  // fneg survives folding when it feeds a call operand
  float n = saturate(-(input.color.x * input.color.w));
  uint u = (uint)(input.color.y * 200.0);    // 200 for color.y == 1
  uint rot = (u << 3) | (u >> 29);           // rotate idiom -> llvm.fshl
  int i1 = (int)(input.color.z * 100.0);     // 100 for color.z == 1
  int i2 = 55;
  int mx = i1 > i2 ? i1 : i2;                // ternary max -> llvm.smax
  return float4(n + 0.5, (rot & 255u) / 255.0, mx / 255.0, 1.0);
}
