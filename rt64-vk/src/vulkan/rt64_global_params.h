/*
 * rt64_global_params — the gParams constant buffer, portably.
 *
 * The original declares this with DirectXMath's XMMATRIX in rt64_view.h. This
 * is the same layout using plain arrays, so it compiles without DirectXMath
 * and can be memcpy'd straight into a uniform buffer.
 *
 * The offsets are asserted against what DXC actually emits for
 * GlobalParams.hlsli. HLSL's 16-byte register packing happens to coincide with
 * natural C++ alignment here because no scalar straddles a register boundary
 * — but that is a property of this particular field order, not a guarantee.
 * Reorder the fields and the asserts are what will catch it.
 */
#ifndef RT64_GLOBAL_PARAMS_H
#define RT64_GLOBAL_PARAMS_H

#include <cstddef>
#include <cstdint>

namespace RT64 {

struct GlobalParams {
    float view[16];
    float viewI[16];
    float prevViewI[16];
    float projection[16];
    float projectionI[16];
    float viewProj[16];
    float prevViewProj[16];
    float cameraU[4];
    float cameraV[4];
    float cameraW[4];
    float viewport[4];
    float resolution[4];
    float ambientBaseColor[4];
    float ambientNoGIColor[4];
    float eyeLightDiffuseColor[4];
    float eyeLightSpecularColor[4];
    float skyDiffuseMultiplier[4];
    float skyHSLModifier[4];
    float pixelJitter[2];
    float skyYawOffset;
    float giDiffuseStrength;
    float giSkyStrength;
    float motionBlurStrength;
    int32_t skyPlaneTexIndex;
    uint32_t randomSeed;
    uint32_t diSamples;
    uint32_t giSamples;
    uint32_t diReproject;
    uint32_t giReproject;
    uint32_t binaryLockMask;
    uint32_t maxLights;
    uint32_t motionBlurSamples;
    uint32_t visualizationMode;
    uint32_t frameCount;
};

/* Verified against `spirv-dis PostProcessPS.spv | grep MemberDecorate Offset`. */
static_assert(offsetof(GlobalParams, view)              ==   0, "gParams layout");
static_assert(offsetof(GlobalParams, viewI)             ==  64, "gParams layout");
static_assert(offsetof(GlobalParams, cameraU)           == 448, "gParams layout");
static_assert(offsetof(GlobalParams, resolution)        == 512, "gParams layout");
static_assert(offsetof(GlobalParams, pixelJitter)       == 624, "gParams layout");
static_assert(offsetof(GlobalParams, skyYawOffset)      == 632, "gParams layout");
static_assert(offsetof(GlobalParams, motionBlurStrength)== 644, "gParams layout");
static_assert(offsetof(GlobalParams, motionBlurSamples) == 680, "gParams layout");
static_assert(offsetof(GlobalParams, frameCount)        == 688, "gParams layout");

/* Binding numbers follow the -fvk-*-shift scheme in CompileShaders.cmake:
   UAVs at 0+, SRVs at 100+, CBVs at 200+, samplers at 300+. */
enum : uint32_t {
    RT64_BINDING_SRV_BASE     = 100,
    RT64_BINDING_CBV_BASE     = 200,
    RT64_BINDING_SAMPLER_BASE = 300,
};

} /* namespace RT64 */

#endif /* RT64_GLOBAL_PARAMS_H */
