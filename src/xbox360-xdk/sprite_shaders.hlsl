// Butterscotch Xbox 360 — 2D Sprite Shaders
// Compiled with fxc.exe:
//   fxc /T vs_2_0 /E VSMain /Fh sprite_vs.h sprite_shaders.hlsl
//   fxc /T ps_2_0 /E PSMain /Fh sprite_ps.h sprite_shaders.hlsl

// Orthographic projection matrix (set per-frame)
float4x4 Projection : register(c0);

struct VSInput {
    float2 Position : POSITION;
    float2 TexCoord : TEXCOORD0;
    float4 Color    : COLOR0;
};

struct VSOutput {
    float4 Position : POSITION;
    float2 TexCoord : TEXCOORD0;
    float4 Color    : COLOR0;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.Position = mul(float4(input.Position, 0.0, 1.0), Projection);
    output.TexCoord = input.TexCoord;
    output.Color    = input.Color;
    return output;
}

sampler2D SpriteSampler : register(s0);

float4 PSMain(VSOutput input) : COLOR0 {
    float4 texel = tex2D(SpriteSampler, input.TexCoord);
    return texel * input.Color;
}
