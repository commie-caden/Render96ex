//
// RT64
//

#ifdef SHADER_AS_STRING
R"raw(
#else
#define MAX_HIT_QUERIES	16

// VULKAN PORT: explicit storage formats.
//
// D3D12 declared these as RWBuffer<float4> and attached a *packed* format to
// the UAV descriptor, letting the hardware convert on read and write. Vulkan
// requires the buffer view format to match what the shader declares, and DXC
// infers Rgba32f from the float4 element type — which would make every one of
// these 16 bytes per element. At 1080p with 17 hit layers that turns
// gHitColor from 141 MB into 564 MB.
//
// [[vk::image_format]] restores the original packing. The formats below are
// exactly the DXGI formats rt64_view.cpp used for the corresponding UAVs.
// These attributes are ignored when compiling to DXIL, so the D3D12 path is
// unaffected.
[[vk::image_format("rgba32f")]]     RWBuffer<float4> gHitDistAndFlow : register(u22);
[[vk::image_format("rgba8")]]       RWBuffer<float4> gHitColor : register(u23);
[[vk::image_format("rgba16snorm")]] RWBuffer<float4> gHitNormal : register(u24);
[[vk::image_format("rgba8")]]       RWBuffer<float4> gHitSpecular : register(u25);
[[vk::image_format("r16ui")]]       RWBuffer<uint> gHitInstanceId : register(u26);

uint getHitBufferIndex(uint hitPos, uint2 pixelIdx, uint2 pixelDims) {
	return (hitPos * pixelDims.y + pixelIdx.y) * pixelDims.x + pixelIdx.x;
}
//)raw"
#endif