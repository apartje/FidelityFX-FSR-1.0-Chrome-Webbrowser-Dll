cbuffer cb : register(b0)
{
	uint4 Const0;
	uint4 Const1;
	uint4 Const2;
	uint4 Const3;
	uint4 Sample;
};

#define A_GPU 1
#define A_HLSL 1
#define SAMPLE_EASU 1
#define SAMPLE_RCAS 1
#define FSR_EASU_F 1
#define FSR_RCAS_F 1
SamplerState samLinearClamp : register(s0);

#include "ffx_a.h"
#include "ffx_fsr1.h"

Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);

AF4 FsrEasuRF(AF2 p) { AF4 res = InputTexture.GatherRed(samLinearClamp, p, int2(0, 0)); return res; }
AF4 FsrEasuGF(AF2 p) { AF4 res = InputTexture.GatherGreen(samLinearClamp, p, int2(0, 0)); return res; } 
AF4 FsrEasuBF(AF2 p) { AF4 res = InputTexture.GatherBlue(samLinearClamp, p, int2(0, 0)); return res; }
AF4 FsrRcasLoadF(ASU2 p) { return InputTexture.Load(int3(ASU2(p), 0)); } 
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

void CurrFilter(int2 pos)
{
	AF3 color;
	FsrRcasF(color.r, color.g, color.b, pos, Const0);
	OutputTexture[pos] = float4(color, 1);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 LocalThreadId : SV_GroupThreadID, uint3 WorkGroupId : SV_GroupID)
{
	AU2 gxy = AU2(LocalThreadId.xy) + AU2(WorkGroupId.xy << 4u);

	CurrFilter(gxy);
}
