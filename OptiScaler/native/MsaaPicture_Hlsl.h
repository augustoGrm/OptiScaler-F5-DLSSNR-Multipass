#pragma once

// The write-back of native/MsaaPictureDx11.cpp: Optical F5Low's single-sample result drawn into a game's multisampled
// back buffer. Compiled ahead of time into native/F5LowShaderBytecode.h by tests/nr_shader_bytecode_smoke.cpp (run it
// with --write after changing anything here; without, it checks the embedded bytecode is current).

namespace MsaaPictureHlsl
{
// One triangle that covers the whole target, from the vertex number alone: the caller binds no vertex buffer and no
// input layout. The pixel shader reads the single-sample picture at the pixel's own position, so a target of the same
// size gets it 1:1. It runs once per pixel and its colour goes to every covered sample, which is all of them for this
// triangle.
inline constexpr const char* kSource = R"HLSL(
Texture2D<float4> Src : register(t0);

float4 FullScreenVS(uint id : SV_VertexID) : SV_Position
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 WriteBackPS(float4 pos : SV_Position) : SV_Target
{
    return Src.Load(int3((int2) pos.xy, 0));
}
)HLSL";
} // namespace MsaaPictureHlsl
