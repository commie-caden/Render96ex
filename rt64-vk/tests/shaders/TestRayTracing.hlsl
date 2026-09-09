//
// TestRayGen — a minimal ray tracing library to validate BLAS/TLAS and the SBT.
//
// Deliberately independent of RT64's own raygen shaders: those need the full
// 34-binding descriptor heap, and the point here is to test the acceleration
// structure and shader binding table machinery in isolation. Barycentrics make
// a hit visually unambiguous — a red/green/blue gradient triangle can only
// appear if the BLAS, TLAS, SBT and hit group are all correct.
//

RaytracingAccelerationStructure SceneBVH : register(t0);
RWTexture2D<float4> gOutput : register(u0);

struct Payload {
    float3 color;
};

[shader("raygeneration")]
void RayGen() {
    uint2 index = DispatchRaysIndex().xy;
    uint2 dims = DispatchRaysDimensions().xy;
    float2 uv = (float2(index) + 0.5f) / float2(dims);

    // Orthographic rays down +Z through a [-1,1] plane.
    RayDesc ray;
    ray.Origin = float3(uv.x * 2.0f - 1.0f, -(uv.y * 2.0f - 1.0f), -1.0f);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin = 0.001f;
    ray.TMax = 100.0f;

    Payload payload;
    payload.color = float3(0.0f, 0.0f, 0.0f);
    TraceRay(SceneBVH, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

    gOutput[index] = float4(payload.color, 1.0f);
}

[shader("miss")]
void Miss(inout Payload payload) {
    // Dark blue background, clearly distinct from any hit colour.
    payload.color = float3(0.04f, 0.05f, 0.12f);
}

[shader("closesthit")]
void ClosestHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    float3 bary = float3(1.0f - attribs.barycentrics.x - attribs.barycentrics.y,
                         attribs.barycentrics.x,
                         attribs.barycentrics.y);
    payload.color = bary;
}
