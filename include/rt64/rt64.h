//
// RT64
//

#ifndef RT64_H_INCLUDED
#define RT64_H_INCLUDED

#if defined(_WIN32) || defined(_WIN64)
#   include <Windows.h>
#else
#   include <dlfcn.h>
#   include <unistd.h>
#   include <stdio.h>
#   include <stdlib.h>
#   include <string.h>
    /* The inspector's message hook is Win32-shaped. On POSIX the game feeds it
       SDL events instead, but the typedef must still exist for ABI parity. */
    typedef unsigned int RT64_MSG;
    typedef unsigned long long RT64_WPARAM;
    typedef long long RT64_LPARAM;
#endif
#include <stdio.h>
#include <string.h>

// Material constants.
#define RT64_MATERIAL_FILTER_POINT				0
#define RT64_MATERIAL_FILTER_LINEAR				1
#define RT64_MATERIAL_ADDR_WRAP					0
#define RT64_MATERIAL_ADDR_MIRROR				1
#define RT64_MATERIAL_ADDR_CLAMP				2
#define RT64_MATERIAL_CC_SHADER_0				0
#define RT64_MATERIAL_CC_SHADER_INPUT_1			1
#define RT64_MATERIAL_CC_SHADER_INPUT_2			2
#define RT64_MATERIAL_CC_SHADER_INPUT_3			3
#define RT64_MATERIAL_CC_SHADER_INPUT_4			4
#define RT64_MATERIAL_CC_SHADER_TEXEL0			5
#define RT64_MATERIAL_CC_SHADER_TEXEL0A			6
#define RT64_MATERIAL_CC_SHADER_TEXEL1			7

// Material attributes.
#define RT64_ATTRIBUTE_NONE							0x0000
#define RT64_ATTRIBUTE_IGNORE_NORMAL_FACTOR			0x0001
#define RT64_ATTRIBUTE_UV_DETAIL_SCALE				0x0002
#define RT64_ATTRIBUTE_REFLECTION_FACTOR			0x0004
#define RT64_ATTRIBUTE_REFLECTION_FRESNEL_FACTOR	0x0008
#define RT64_ATTRIBUTE_REFLECTION_SHINE_FACTOR		0x0010
#define RT64_ATTRIBUTE_REFRACTION_FACTOR			0x0020
#define RT64_ATTRIBUTE_SPECULAR_COLOR				0x0040
#define RT64_ATTRIBUTE_SPECULAR_EXPONENT			0x0080
#define RT64_ATTRIBUTE_SOLID_ALPHA_MULTIPLIER		0x0100
#define RT64_ATTRIBUTE_SHADOW_ALPHA_MULTIPLIER		0x0200
#define RT64_ATTRIBUTE_DEPTH_BIAS					0x0400
#define RT64_ATTRIBUTE_SHADOW_RAY_BIAS				0x0800
#define RT64_ATTRIBUTE_SELF_LIGHT					0x1000
#define RT64_ATTRIBUTE_LIGHT_GROUP_MASK_BITS		0x2000
#define RT64_ATTRIBUTE_DIFFUSE_COLOR_MIX			0x4000

// Mesh flags.
#define RT64_MESH_RAYTRACE_ENABLED				0x1
#define RT64_MESH_RAYTRACE_UPDATABLE			0x2
#define RT64_MESH_RAYTRACE_FAST_TRACE			0x4
#define RT64_MESH_RAYTRACE_COMPACT				0x8

// Shader flags.
#define RT64_SHADER_FILTER_POINT				0x0
#define RT64_SHADER_FILTER_LINEAR				0x1
#define RT64_SHADER_ADDRESSING_WRAP				0x0
#define RT64_SHADER_ADDRESSING_MIRROR			0x1
#define RT64_SHADER_ADDRESSING_CLAMP			0x2
#define RT64_SHADER_RASTER_ENABLED				0x1
#define RT64_SHADER_RAYTRACE_ENABLED			0x2
#define RT64_SHADER_NORMAL_MAP_ENABLED			0x4
#define RT64_SHADER_SPECULAR_MAP_ENABLED		0x8

// Instance flags.
#define RT64_INSTANCE_RASTER_BACKGROUND			0x1
#define RT64_INSTANCE_DISABLE_BACKFACE_CULLING	0x2

// Light flags.
#define RT64_LIGHT_GROUP_MASK_ALL				0xFFFFFFFF
#define RT64_LIGHT_GROUP_DEFAULT				0x1
#define RT64_LIGHT_MAX_SAMPLES					128

// View attributes.
#define RT64_UPSCALER_OFF						0x0
#define RT64_UPSCALER_AUTO						0x1
#define RT64_UPSCALER_DLSS						0x2
#define RT64_UPSCALER_FSR						0x3
#define RT64_UPSCALER_XESS						0x4
#define RT64_UPSCALER_MODE_AUTO					0x0
#define RT64_UPSCALER_MODE_ULTRA_PERFORMANCE	0x1
#define RT64_UPSCALER_MODE_PERFORMANCE			0x2
#define RT64_UPSCALER_MODE_BALANCED				0x3
#define RT64_UPSCALER_MODE_QUALITY				0x4
#define RT64_UPSCALER_MODE_ULTRA_QUALITY		0x5
#define RT64_UPSCALER_MODE_NATIVE				0x6

// Texture formats.
#define RT64_TEXTURE_FORMAT_RGBA8				0x1
#define RT64_TEXTURE_FORMAT_DDS					0x2

// Forward declaration of types.
typedef struct RT64_DEVICE RT64_DEVICE;
typedef struct RT64_VIEW RT64_VIEW;
typedef struct RT64_SCENE RT64_SCENE;
typedef struct RT64_INSTANCE RT64_INSTANCE;
typedef struct RT64_MESH RT64_MESH;
typedef struct RT64_TEXTURE RT64_TEXTURE;
typedef struct RT64_SHADER RT64_SHADER;
typedef struct RT64_INSPECTOR RT64_INSPECTOR;

typedef struct {
	float x, y;
} RT64_VECTOR2;

typedef struct {
	float x, y, z;
} RT64_VECTOR3;

typedef struct {
	float x, y, z, w;
} RT64_VECTOR4;

typedef struct {
	float m[4][4];
} RT64_MATRIX4;

typedef struct {
	int x, y, w, h;
} RT64_RECT;

typedef struct {
	int diffuseTexIndex;
	int normalTexIndex;
	int specularTexIndex;
	float ignoreNormalFactor;
	float uvDetailScale;
	float reflectionFactor;
	float reflectionFresnelFactor;
	float reflectionShineFactor;
	float refractionFactor;
	RT64_VECTOR3 specularColor;
	float specularExponent;
	float solidAlphaMultiplier;
	float shadowAlphaMultiplier;
	float depthBias;
	float shadowRayBias;
	RT64_VECTOR3 selfLight;
	unsigned int lightGroupMaskBits;
	RT64_VECTOR3 fogColor;
	RT64_VECTOR4 diffuseColorMix;
	float fogMul;
	float fogOffset;
	unsigned int fogEnabled;
	float lockMask;

	// Flag containing all attributes that are actually used by this material.
	int enabledAttributes;
} RT64_MATERIAL;

// Light
typedef struct {
	RT64_VECTOR3 position;
	RT64_VECTOR3 diffuseColor;
	float attenuationRadius;
	float pointRadius;
	RT64_VECTOR3 specularColor;
	float shadowOffset;
	float attenuationExponent;
	float flickerIntensity;
	unsigned int groupBits;
} RT64_LIGHT;

typedef struct {
	RT64_VECTOR3 ambientBaseColor;
	RT64_VECTOR3 ambientNoGIColor;
	RT64_VECTOR3 eyeLightDiffuseColor;
	RT64_VECTOR3 eyeLightSpecularColor;
	RT64_VECTOR3 skyDiffuseMultiplier;
	RT64_VECTOR3 skyHSLModifier;
	float skyYawOffset;
	float giDiffuseStrength;
	float giSkyStrength;
} RT64_SCENE_DESC;

typedef struct {
	float resolutionScale;
	float motionBlurStrength;
	unsigned int diSamples;
	unsigned int giSamples;
	unsigned int maxLights;
	unsigned char upscaler;
	unsigned char upscalerMode;
	float upscalerSharpness;
	bool denoiserEnabled;
} RT64_VIEW_DESC;

typedef struct {
	RT64_MESH *mesh;
	RT64_MATRIX4 transform;
	RT64_MATRIX4 previousTransform;
	RT64_TEXTURE *diffuseTexture;
	RT64_TEXTURE *normalTexture;
	RT64_TEXTURE *specularTexture;
	RT64_SHADER *shader;
	RT64_MATERIAL material;
	RT64_RECT scissorRect;
	RT64_RECT viewportRect;
	unsigned int flags;
} RT64_INSTANCE_DESC;

typedef struct {
	void *bytes;
	int byteCount;
	int format;
	int width;
	int height;
	int rowPitch;
} RT64_TEXTURE_DESC;

inline void RT64_ApplyMaterialAttributes(RT64_MATERIAL *dst, RT64_MATERIAL *src) {
	if (src->enabledAttributes & RT64_ATTRIBUTE_IGNORE_NORMAL_FACTOR) {
		dst->ignoreNormalFactor = src->ignoreNormalFactor;
	}

	if (src->enabledAttributes & RT64_ATTRIBUTE_UV_DETAIL_SCALE) {
		dst->uvDetailScale = src->uvDetailScale;
	}

	if (src->enabledAttributes & RT64_ATTRIBUTE_REFLECTION_FACTOR) {
		dst->reflectionFactor = src->reflectionFactor;
	}

	if (src->enabledAttributes & RT64_ATTRIBUTE_REFLECTION_FRESNEL_FACTOR) {
		dst->reflectionFresnelFactor = src->reflectionFresnelFactor;
	}

	if (src->enabledAttributes & RT64_ATTRIBUTE_REFLECTION_SHINE_FACTOR) {
		dst->reflectionShineFactor = src->reflectionShineFactor;
	}

	if (src->enabledAttributes & RT64_ATTRIBUTE_REFRACTION_FACTOR) {
		dst->refractionFactor = src->refractionFactor;
	}

	if (src->enabledAttributes & RT64_ATTRIBUTE_SPECULAR_COLOR) {
		dst->specularColor = src->specularColor;
	}

	if (src->enabledAttributes & RT64_ATTRIBUTE_SPECULAR_EXPONENT) {
		dst->specularExponent = src->specularExponent;
	}

	if (src->enabledAttributes & RT64_ATTRIBUTE_SOLID_ALPHA_MULTIPLIER) {
		dst->solidAlphaMultiplier = src->solidAlphaMultiplier;
	}

	if (src->enabledAttributes & RT64_ATTRIBUTE_SHADOW_ALPHA_MULTIPLIER) {
		dst->shadowAlphaMultiplier = src->shadowAlphaMultiplier;
	}

	if (src->enabledAttributes & RT64_ATTRIBUTE_DEPTH_BIAS) {
		dst->depthBias = src->depthBias;
	}

	if (src->enabledAttributes & RT64_ATTRIBUTE_SHADOW_RAY_BIAS) {
		dst->shadowRayBias = src->shadowRayBias;
	}

	if (src->enabledAttributes & RT64_ATTRIBUTE_SELF_LIGHT) {
		dst->selfLight = src->selfLight;
	}

	if (src->enabledAttributes & RT64_ATTRIBUTE_LIGHT_GROUP_MASK_BITS) {
		dst->lightGroupMaskBits = src->lightGroupMaskBits;
	}

	if (src->enabledAttributes & RT64_ATTRIBUTE_DIFFUSE_COLOR_MIX) {
		dst->diffuseColorMix = src->diffuseColorMix;
	}
}

// Internal function pointer types.
typedef const char *(*GetLastErrorPtr)();
typedef RT64_DEVICE* (*CreateDevicePtr)(void *hwnd);
typedef void (*DestroyDevicePtr)(RT64_DEVICE* device);
typedef void (*DrawDevicePtr)(RT64_DEVICE *device, int vsyncInterval, float deltaTimeMs);
typedef RT64_VIEW* (*CreateViewPtr)(RT64_SCENE* scenePtr);
typedef void (*SetViewPerspectivePtr)(RT64_VIEW *viewPtr, RT64_MATRIX4 viewMatrix, float fovRadians, float nearDist, float farDist, bool canReproject);
typedef void (*SetViewDescriptionPtr)(RT64_VIEW *viewPtr, RT64_VIEW_DESC viewDesc);
typedef void (*SetViewSkyPlanePtr)(RT64_VIEW *viewPtr, RT64_TEXTURE *texturePtr);
typedef RT64_INSTANCE* (*GetViewRaytracedInstanceAtPtr)(RT64_VIEW *viewPtr, int x, int y);
typedef bool (*GetViewUpscalerSupportPtr)(RT64_VIEW *viewPtr, char upscaler);
typedef void (*DestroyViewPtr)(RT64_VIEW* viewPtr);
typedef RT64_SCENE* (*CreateScenePtr)(RT64_DEVICE* devicePtr);
typedef void (*SetSceneDescriptionPtr)(RT64_SCENE* scenePtr, RT64_SCENE_DESC sceneDesc);
typedef void (*SetSceneLightsPtr)(RT64_SCENE* scenePtr, RT64_LIGHT* lightArray, int lightCount);
typedef void (*DestroyScenePtr)(RT64_SCENE* scenePtr);
typedef RT64_MESH* (*CreateMeshPtr)(RT64_DEVICE* devicePtr, int flags);
typedef void (*SetMeshPtr)(RT64_MESH* meshPtr, void* vertexArray, int vertexCount, int vertexStride, unsigned int* indexArray, int indexCount);
typedef void (*DestroyMeshPtr)(RT64_MESH* meshPtr);
typedef RT64_SHADER *(*CreateShaderPtr)(RT64_DEVICE *devicePtr, unsigned int shaderId, unsigned int filter, unsigned int hAddr, unsigned int vAddr, int flags);
typedef void (*DestroyShaderPtr)(RT64_SHADER *shaderPtr);
typedef RT64_INSTANCE* (*CreateInstancePtr)(RT64_SCENE* scenePtr);
typedef void (*SetInstanceDescriptionPtr)(RT64_INSTANCE* instancePtr, RT64_INSTANCE_DESC instanceDesc);
typedef void (*DestroyInstancePtr)(RT64_INSTANCE* instancePtr);
typedef RT64_TEXTURE* (*CreateTexturePtr)(RT64_DEVICE* devicePtr, RT64_TEXTURE_DESC textureDesc);
typedef void (*DestroyTexturePtr)(RT64_TEXTURE* texture);
typedef RT64_INSPECTOR* (*CreateInspectorPtr)(RT64_DEVICE* devicePtr);
#if defined(_WIN32) || defined(_WIN64)
typedef bool (*HandleMessageInspectorPtr)(RT64_INSPECTOR* inspectorPtr, UINT msg, WPARAM wParam, LPARAM lParam);
#else
typedef bool (*HandleMessageInspectorPtr)(RT64_INSPECTOR* inspectorPtr, RT64_MSG msg, RT64_WPARAM wParam, RT64_LPARAM lParam);
#endif
typedef void (*SetSceneInspectorPtr)(RT64_INSPECTOR* inspectorPtr, RT64_SCENE_DESC* sceneDesc);
typedef void (*SetMaterialInspectorPtr)(RT64_INSPECTOR* inspectorPtr, RT64_MATERIAL* material, const char *materialName);
typedef void (*SetLightsInspectorPtr)(RT64_INSPECTOR* inspectorPtr, RT64_LIGHT* lights, int *lightCount, int maxLightCount);
typedef void (*PrintClearInspectorPtr)(RT64_INSPECTOR *inspectorPtr);
typedef void (*PrintMessageInspectorPtr)(RT64_INSPECTOR* inspectorPtr, const char* message);
typedef void (*DestroyInspectorPtr)(RT64_INSPECTOR* inspectorPtr);

// Stores all the function pointers used in the RT64 library.
typedef struct {
	void *handle;
	GetLastErrorPtr GetLastError;
	CreateDevicePtr CreateDevice;
	DestroyDevicePtr DestroyDevice;
#ifndef RT64_MINIMAL
	DrawDevicePtr DrawDevice;
	CreateViewPtr CreateView;
	SetViewPerspectivePtr SetViewPerspective;
	SetViewDescriptionPtr SetViewDescription;
	SetViewSkyPlanePtr SetViewSkyPlane;
	GetViewRaytracedInstanceAtPtr GetViewRaytracedInstanceAt;
	GetViewUpscalerSupportPtr GetViewUpscalerSupport;
	DestroyViewPtr DestroyView;
	CreateScenePtr CreateScene;
	SetSceneDescriptionPtr SetSceneDescription;
	SetSceneLightsPtr SetSceneLights;
	DestroyScenePtr DestroyScene;
	CreateMeshPtr CreateMesh;
	SetMeshPtr SetMesh;
	DestroyMeshPtr DestroyMesh;
	CreateShaderPtr CreateShader;
	DestroyShaderPtr DestroyShader;
	CreateInstancePtr CreateInstance;
	SetInstanceDescriptionPtr SetInstanceDescription;
	DestroyInstancePtr DestroyInstance;
	CreateTexturePtr CreateTexture;
	DestroyTexturePtr DestroyTexture;
	CreateInspectorPtr CreateInspector;
	HandleMessageInspectorPtr HandleMessageInspector;
	PrintClearInspectorPtr PrintClearInspector;
	PrintMessageInspectorPtr PrintMessageInspector;
	SetSceneInspectorPtr SetSceneInspector;
	SetMaterialInspectorPtr SetMaterialInspector;
	SetLightsInspectorPtr SetLightsInspector;
	DestroyInspectorPtr DestroyInspector;
#endif
} RT64_LIBRARY;


// Define RT64_DEBUG for loading the debug DLL.
#if defined(_WIN32) || defined(_WIN64)
#   define RT64_SYM(h, n) GetProcAddress((HMODULE)(h), n)
#else
#   define RT64_SYM(h, n) dlsym((h), n)
#endif

inline RT64_LIBRARY RT64_LoadLibrary() {
	/* Zero first: on a failed load the function pointers are never assigned,
	   and callers should see null rather than stack garbage. memset rather
	   than {0} so this stays warning-free from both C and C++. */
	RT64_LIBRARY lib;
	memset(&lib, 0, sizeof(RT64_LIBRARY));

#if defined(_WIN32) || defined(_WIN64)
#   if defined(RT64_MINIMAL)
	lib.handle = (void *)LoadLibrary(TEXT("rt64libm.dll"));
#   elif defined(RT64_DEBUG)
	lib.handle = (void *)LoadLibrary(TEXT("rt64libd.dll"));
#   else
	lib.handle = (void *)LoadLibrary(TEXT("rt64lib.dll"));
#   endif
#else
	/* Windows LoadLibrary searches the application's own directory, so the
	   faithful equivalent resolves paths against the executable rather than
	   the working directory. Launching as ./build/us_pc/sm64.us.f3dex2e from
	   the project root otherwise fails, because "./librt64.so" is relative to
	   wherever the shell happens to be. */
	{
		char exeDir[4096];
		ssize_t len = readlink("/proc/self/exe", exeDir, sizeof(exeDir) - 1);
		if (len > 0) {
			exeDir[len] = '\0';
			char *slash = strrchr(exeDir, '/');
			if (slash != NULL) {
				*slash = '\0';
			}
		} else {
			exeDir[0] = '\0';
		}

		const char *envPath = getenv("RT64_LIB");
		char beside[4096];
		if (exeDir[0] != '\0') {
			snprintf(beside, sizeof(beside), "%s/librt64.so", exeDir);
		} else {
			beside[0] = '\0';
		}

		const char *candidates[4];
		int count = 0;
		if (envPath != NULL)   { candidates[count++] = envPath; }
		if (beside[0] != '\0') { candidates[count++] = beside; }
		candidates[count++] = "librt64.so";     /* loader search path */
		candidates[count++] = "./librt64.so";   /* working directory */

		lib.handle = 0;
		for (int i = 0; (i < count) && (lib.handle == 0); i++) {
			lib.handle = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
			if (lib.handle == 0) {
				/* Keep the last error: a library that exists but fails to load
				   (a missing dependency, say) reports something far more
				   useful than "No such file or directory". */
				const char *why = dlerror();
				if (why != NULL) {
					fprintf(stderr, "RT64: %s\n", why);
				}
			}
		}
	}
#endif

	if (lib.handle != 0) {
		lib.GetLastError = (GetLastErrorPtr)(RT64_SYM(lib.handle, "RT64_GetLastError"));
		lib.CreateDevice = (CreateDevicePtr)(RT64_SYM(lib.handle, "RT64_CreateDevice"));
		lib.DestroyDevice = (DestroyDevicePtr)(RT64_SYM(lib.handle, "RT64_DestroyDevice"));

#ifndef RT64_MINIMAL
		lib.DrawDevice = (DrawDevicePtr)(RT64_SYM(lib.handle, "RT64_DrawDevice"));
		lib.CreateView = (CreateViewPtr)(RT64_SYM(lib.handle, "RT64_CreateView"));
		lib.SetViewPerspective = (SetViewPerspectivePtr)(RT64_SYM(lib.handle, "RT64_SetViewPerspective"));
		lib.SetViewDescription = (SetViewDescriptionPtr)(RT64_SYM(lib.handle, "RT64_SetViewDescription"));
		lib.SetViewSkyPlane = (SetViewSkyPlanePtr)(RT64_SYM(lib.handle, "RT64_SetViewSkyPlane"));
		lib.GetViewRaytracedInstanceAt = (GetViewRaytracedInstanceAtPtr)(RT64_SYM(lib.handle, "RT64_GetViewRaytracedInstanceAt"));
		lib.GetViewUpscalerSupport = (GetViewUpscalerSupportPtr)(RT64_SYM(lib.handle, "RT64_GetViewUpscalerSupport"));
		lib.DestroyView = (DestroyViewPtr)(RT64_SYM(lib.handle, "RT64_DestroyView"));
		lib.CreateScene = (CreateScenePtr)(RT64_SYM(lib.handle, "RT64_CreateScene"));
		lib.SetSceneDescription = (SetSceneDescriptionPtr)(RT64_SYM(lib.handle, "RT64_SetSceneDescription"));
		lib.SetSceneLights = (SetSceneLightsPtr)(RT64_SYM(lib.handle, "RT64_SetSceneLights"));
		lib.DestroyScene = (DestroyScenePtr)(RT64_SYM(lib.handle, "RT64_DestroyScene"));
		lib.CreateMesh = (CreateMeshPtr)(RT64_SYM(lib.handle, "RT64_CreateMesh"));
		lib.SetMesh = (SetMeshPtr)(RT64_SYM(lib.handle, "RT64_SetMesh"));
		lib.DestroyMesh = (DestroyMeshPtr)(RT64_SYM(lib.handle, "RT64_DestroyMesh"));
		lib.CreateShader = (CreateShaderPtr)(RT64_SYM(lib.handle, "RT64_CreateShader"));
		lib.DestroyShader = (DestroyShaderPtr)(RT64_SYM(lib.handle, "RT64_DestroyShader"));
		lib.CreateInstance = (CreateInstancePtr)(RT64_SYM(lib.handle, "RT64_CreateInstance"));
		lib.SetInstanceDescription = (SetInstanceDescriptionPtr)(RT64_SYM(lib.handle, "RT64_SetInstanceDescription"));
		lib.DestroyInstance = (DestroyInstancePtr)(RT64_SYM(lib.handle, "RT64_DestroyInstance"));
		lib.CreateTexture = (CreateTexturePtr)(RT64_SYM(lib.handle, "RT64_CreateTexture"));
		lib.DestroyTexture = (DestroyTexturePtr)(RT64_SYM(lib.handle, "RT64_DestroyTexture"));
		lib.CreateInspector = (CreateInspectorPtr)(RT64_SYM(lib.handle, "RT64_CreateInspector"));
		lib.HandleMessageInspector = (HandleMessageInspectorPtr)(RT64_SYM(lib.handle, "RT64_HandleMessageInspector"));
		lib.SetSceneInspector = (SetSceneInspectorPtr)(RT64_SYM(lib.handle, "RT64_SetSceneInspector"));
		lib.SetMaterialInspector = (SetMaterialInspectorPtr)(RT64_SYM(lib.handle, "RT64_SetMaterialInspector"));
		lib.SetLightsInspector = (SetLightsInspectorPtr)(RT64_SYM(lib.handle, "RT64_SetLightsInspector"));
		lib.PrintClearInspector = (PrintClearInspectorPtr)(RT64_SYM(lib.handle, "RT64_PrintClearInspector"));
		lib.PrintMessageInspector = (PrintMessageInspectorPtr)(RT64_SYM(lib.handle, "RT64_PrintMessageInspector"));
		lib.DestroyInspector = (DestroyInspectorPtr)(RT64_SYM(lib.handle, "RT64_DestroyInspector"));
#endif
	}
	else {
#if defined(_WIN32) || defined(_WIN64)
		char errorMessage[256];
		FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), errorMessage, sizeof(errorMessage), NULL);
		fprintf(stderr, "Error when loading library: %s\n", errorMessage);
#else
		fprintf(stderr, "Error when loading library: %s\n", dlerror());
#endif
	}

	return lib;
}

inline void RT64_UnloadLibrary(RT64_LIBRARY lib) {
#if defined(_WIN32) || defined(_WIN64)
	FreeLibrary((HMODULE)lib.handle);
#else
	if (lib.handle != 0) {
		dlclose(lib.handle);
	}
#endif
}

#endif