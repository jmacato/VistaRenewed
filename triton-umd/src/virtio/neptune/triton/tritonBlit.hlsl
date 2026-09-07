// Stretch/convert blit for the DXGI Blt DDI (tritonDxgiBltCommon).
// Fullscreen triangle; the viewport is the dst rect, B.st maps the
// triangle's [0,1]x[0,1] quad UV onto the source box in texel-normalised
// source-texture coordinates.

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSBlit(uint id : SV_VertexID)
{
    VSOut o;
    float2 quv = float2((id << 1) & 2, id & 2);
    o.pos = float4(quv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = quv;
    return o;
}

Texture2D    t : register(t0);
SamplerState s : register(s0);
cbuffer B : register(b0) { float4 st; };   // xy = scale, zw = offset

float4 PSBlit(VSOut i) : SV_Target
{
    return t.SampleLevel(s, i.uv * st.xy + st.zw, 0.0);
}
