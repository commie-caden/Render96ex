//
// RT64
//

#pragma once

#include <cstring>

#define SHADER_AS_STRING

const char GlobalHitBuffersHLSLI[] =
#include "shaders/GlobalHitBuffers.hlsli"
;

const char InstancesHLSLI[] =
#include "shaders/Instances.hlsli"
;

const char MaterialsHLSLI[] =
#include "shaders/Materials.hlsli"
;

const char RandomHLSLI[] =
#include "shaders/Random.hlsli"
;

const char RayHLSLI[] =
#include "shaders/Ray.hlsli"
;

const char TexturesHLSLI[] =
#include "shaders/Textures.hlsli"
;

const char GlobalParamsHLSLI[] =
#include "shaders/GlobalParams.hlsli"
;

/* The .hlsli files double as C++ raw string literals: R"raw( opens in
   translation phase 3, so the #else and #endif inside are swallowed as text
   rather than acting as directives. That leaves a stray "#else" at the start
   of every embedded string, which must be removed before the text is handed
   to the shader compiler.

   The original skipped a fixed 6 bytes — &x[strlen("#else\n")] — which is
   correct only while R"raw( is followed by exactly one newline and then
   #else. A blank line added anywhere before #else in any .hlsli would
   silently corrupt every runtime-generated shader, with no compile error on
   the C++ side. Searching for the marker instead survives reformatting. */
inline const char *rt64_skip_hlsli_prologue(const char *text) {
    const char *marker = strstr(text, "#else\n");
    return (marker != nullptr) ? marker + 6 : text;
}

#define INCLUDE_HLSLI(x) rt64_skip_hlsli_prologue(x)