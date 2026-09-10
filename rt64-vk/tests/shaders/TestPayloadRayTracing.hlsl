//
// TestPayloadRayTracing — does a payload write survive IgnoreHit()?
//
// RT64's anyhit shaders count intersections into the ray payload and then call
// IgnoreHit() so traversal continues through transparent layers. PrimaryRayGen
// reads that count afterwards to decide how many hits to process. If the
// increment does not survive the terminator, every hit is recorded into the
// hit buffers and then ignored, which is exactly the symptom being chased.
//
// This isolates that one question from everything else.
//
RaytracingAccelerationStructure SceneBVH : register(t0);
RWTexture2D<float4> gOutput : register(u0);
// A storage texel buffer written by the anyhit and read back by the raygen
// after traversal, exactly as RT64 uses gHitColor and friends.
[[vk::image_format("rgba32f")]] RWBuffer<float4> gProbe : register(u1);

// RT64's payload shape: a counter plus four float3s. The single-uint version
// already proved the mechanism works, so this checks whether the larger
// struct behaves the same.
struct RayDiffProbe { float3 dOdx; float3 dOdy; float3 dDdx; float3 dDdy; };
struct CountPayload { uint hits; RayDiffProbe rayDiff; };

[shader("raygeneration")]
void PayloadRayGen() {
    uint2 index = DispatchRaysIndex().xy;
    uint2 dims = DispatchRaysDimensions().xy;
    float2 uv = (float2(index) + 0.5f) / float2(dims);

    RayDesc ray;
    ray.Origin = float3(uv.x * 2.0f - 1.0f, -(uv.y * 2.0f - 1.0f), -1.0f);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin = 0.001f;
    ray.TMax = 100.0f;

    CountPayload payload;
    payload.hits = 0;
    payload.rayDiff.dOdx = float3(0.0f, 0.0f, 0.0f);
    payload.rayDiff.dOdy = float3(0.0f, 0.0f, 0.0f);
    payload.rayDiff.dDdx = float3(0.0f, 0.0f, 0.0f);
    payload.rayDiff.dDdy = float3(0.0f, 0.0f, 0.0f);
    uint probeIndex = index.y * dims.x + index.x;
    gProbe[probeIndex] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    // FORCE_NON_OPAQUE and SKIP_CLOSEST_HIT match how PrimaryRayGen traces.
    TraceRay(SceneBVH, RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
             0xFF, 0, 0, 0, ray, payload);

    // Red   = the payload count the raygen observes after traversal.
    // Green = a value the anyhit wrote to a texel buffer, read back here.
    //         RT64 depends on exactly this: its raygen reads gHitColor and
    //         friends immediately after TraceRay, with no barrier possible.
    float written = gProbe[probeIndex].x;
    gOutput[index] = float4((float)payload.hits, written, 0.0f, 1.0f);
}

[shader("miss")]
void PayloadMiss(inout CountPayload payload) {
    // No-op, exactly like RT64's SurfaceMiss.
}

[shader("anyhit")]
void PayloadAnyHit(inout CountPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dims = DispatchRaysDimensions().xy;
    gProbe[idx.y * dims.x + idx.x] = float4(1.0f, 0.0f, 0.0f, 1.0f);
    ++payload.hits;
    IgnoreHit();
}
