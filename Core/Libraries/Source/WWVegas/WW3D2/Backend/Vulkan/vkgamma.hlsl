// TheSuperHackers @feature O141 The brightness pass of the Vulkan renderer (as the gamma pass of dx12device.cpp):
// one triangle over the back buffer reads a copy of it (t0) and the 256-entry ramp (t1) and writes the color through the
// ramp, alpha unchanged. Compiled to SPIR-V when the build is configured (cmake/renderer.cmake), not at run time.
Texture2D<float4> Scene : register(t0);
Texture2D<float4> Ramp : register(t1);

struct VSOut
{
	float4 Position : SV_Position;
};

VSOut VSMain(uint id : SV_VertexID)
{
	float2 uv = float2((id << 1) & 2, id & 2);
	VSOut o;
	o.Position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return o;
}

float4 PSMain(VSOut i) : SV_Target
{
	float4 c = Scene.Load(int3(i.Position.xy, 0));
	int3 index = int3(round(saturate(c.rgb) * 255.0));
	float r = Ramp.Load(int3(index.r, 0, 0)).r;
	float g = Ramp.Load(int3(index.g, 0, 0)).g;
	float b = Ramp.Load(int3(index.b, 0, 0)).b;
	return float4(r, g, b, c.a);
}
