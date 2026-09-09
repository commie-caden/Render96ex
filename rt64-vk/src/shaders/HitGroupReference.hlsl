//
// HitGroupReference — a reflection stand-in for the runtime-generated hit groups.
//
// RT64 generates its hit groups per material at runtime (rt64_shader.cpp), so
// the descriptor reflection that builds the RayTracing set layout never sees
// them. Without this file the layout is wrong in two ways: it omits the
// bindings only hit groups use (instanceTransforms, instanceMaterials, the
// and its stageFlags carry only RAYGEN and MISS, so the driver rejects any
// pipeline whose closesthit or anyhit touches a shared binding.
//
// This declares the same resources the generator emits and references them
// from a dummy hit group, purely so reflection records them with the right
// stages. It is never dispatched — no SBT record points at it.
//
// IT MUST MIRROR WHAT rt64_shader.cpp EMITS. If the generator gains or drops a
// binding, update this too; rt64_pipeline_test is the safety net, since the
// validation layers reject a mismatched layout at pipeline creation.
//

// Mesh geometry reaches the hit groups through the shader record buffer, not a
// descriptor — see the VULKAN PORT note in generateSurfaceHitGroup. A shader
// record is not part of the descriptor set, so this contributes no binding; it
// is declared here only so the reference compiles the same way the generated
// hit groups do.
struct RT64MeshAddresses { uint64_t vertexAddress; uint64_t indexAddress; };
[[vk::shader_record_ext]] ConstantBuffer<RT64MeshAddresses> gMeshAddresses;

#include "Materials.hlsli"
#include "Instances.hlsli"
#include "GlobalHitBuffers.hlsli"
#include "Textures.hlsli"

// The generator emits one sampler per filter/addressing combination, at
// register s(1 + filter*9 + hAddr*3 + vAddr) — see uniqueSamplerRegisterIndex.
// That is 18 possible registers, s1 through s18, and a material references
// exactly one of them. D3D12 declared these as static samplers in the root
// signature; Vulkan's equivalent is an immutable sampler baked into the
// descriptor set layout, which DescriptorLayouts creates from the same
// formula. All 18 are declared and referenced here so reflection records
// every one, whichever combinations a scene happens to use.
SamplerState gSampler1  : register(s1);
SamplerState gSampler2  : register(s2);
SamplerState gSampler3  : register(s3);
SamplerState gSampler4  : register(s4);
SamplerState gSampler5  : register(s5);
SamplerState gSampler6  : register(s6);
SamplerState gSampler7  : register(s7);
SamplerState gSampler8  : register(s8);
SamplerState gSampler9  : register(s9);
SamplerState gSampler10 : register(s10);
SamplerState gSampler11 : register(s11);
SamplerState gSampler12 : register(s12);
SamplerState gSampler13 : register(s13);
SamplerState gSampler14 : register(s14);
SamplerState gSampler15 : register(s15);
SamplerState gSampler16 : register(s16);
SamplerState gSampler17 : register(s17);
SamplerState gSampler18 : register(s18);

struct HitGroupRefPayload { float4 colorAndDistance; };
struct HitGroupRefAttributes { float2 barycentrics; };

[shader("closesthit")]
void HitGroupReferenceClosestHit(inout HitGroupRefPayload payload,
                                 in HitGroupRefAttributes attrib) {
    // Touch every shared resource so nothing is optimised away before
    // reflection sees it.
    uint index = vk::RawBufferLoad<uint>(gMeshAddresses.indexAddress, 4);
    float3 pos = float3(
        vk::RawBufferLoad<float>(gMeshAddresses.vertexAddress + index * 44, 4),
        vk::RawBufferLoad<float>(gMeshAddresses.vertexAddress + index * 44 + 4, 4),
        vk::RawBufferLoad<float>(gMeshAddresses.vertexAddress + index * 44 + 8, 4));
    float4 xf = mul(instanceTransforms[0].objectToWorld, float4(pos, 1.0f));
    /* Reference gTextures and every sampler so reflection records them with
       the closesthit and anyhit stages the generated hit groups actually use. */
    int texIndex = instanceMaterials[0].diffuseTexIndex;
    float4 acc = float4(0.0f, 0.0f, 0.0f, 0.0f);
    acc += gTextures[NonUniformResourceIndex(texIndex)].SampleLevel(gSampler1,  attrib.barycentrics, 0);
    acc += gTextures[NonUniformResourceIndex(texIndex)].SampleLevel(gSampler2,  attrib.barycentrics, 0);
    acc += gTextures[NonUniformResourceIndex(texIndex)].SampleLevel(gSampler3,  attrib.barycentrics, 0);
    acc += gTextures[NonUniformResourceIndex(texIndex)].SampleLevel(gSampler4,  attrib.barycentrics, 0);
    acc += gTextures[NonUniformResourceIndex(texIndex)].SampleLevel(gSampler5,  attrib.barycentrics, 0);
    acc += gTextures[NonUniformResourceIndex(texIndex)].SampleLevel(gSampler6,  attrib.barycentrics, 0);
    acc += gTextures[NonUniformResourceIndex(texIndex)].SampleLevel(gSampler7,  attrib.barycentrics, 0);
    acc += gTextures[NonUniformResourceIndex(texIndex)].SampleLevel(gSampler8,  attrib.barycentrics, 0);
    acc += gTextures[NonUniformResourceIndex(texIndex)].SampleLevel(gSampler9,  attrib.barycentrics, 0);
    acc += gTextures[NonUniformResourceIndex(texIndex)].SampleLevel(gSampler10, attrib.barycentrics, 0);
    acc += gTextures[NonUniformResourceIndex(texIndex)].SampleLevel(gSampler11, attrib.barycentrics, 0);
    acc += gTextures[NonUniformResourceIndex(texIndex)].SampleLevel(gSampler12, attrib.barycentrics, 0);
    acc += gTextures[NonUniformResourceIndex(texIndex)].SampleLevel(gSampler13, attrib.barycentrics, 0);
    acc += gTextures[NonUniformResourceIndex(texIndex)].SampleLevel(gSampler14, attrib.barycentrics, 0);
    acc += gTextures[NonUniformResourceIndex(texIndex)].SampleLevel(gSampler15, attrib.barycentrics, 0);
    acc += gTextures[NonUniformResourceIndex(texIndex)].SampleLevel(gSampler16, attrib.barycentrics, 0);
    acc += gTextures[NonUniformResourceIndex(texIndex)].SampleLevel(gSampler17, attrib.barycentrics, 0);
    acc += gTextures[NonUniformResourceIndex(texIndex)].SampleLevel(gSampler18, attrib.barycentrics, 0);
    payload.colorAndDistance = xf + acc;
}

[shader("anyhit")]
void HitGroupReferenceAnyHit(inout HitGroupRefPayload payload,
                             in HitGroupRefAttributes attrib) {
    uint index = getHitBufferIndex(0, uint2(0, 0), uint2(1, 1));
    gHitDistAndFlow[index] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    gHitColor[index] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    gHitNormal[index] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    gHitSpecular[index] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    gHitInstanceId[index] = 0;
    /* Real anyhit shaders alpha-test against the diffuse texture. */
    float4 tex = gTextures[NonUniformResourceIndex(0)]
                     .SampleLevel(gSampler10, attrib.barycentrics, 0);
    payload.colorAndDistance = float4(attrib.barycentrics, 0.0f, 0.0f) + tex;
}
