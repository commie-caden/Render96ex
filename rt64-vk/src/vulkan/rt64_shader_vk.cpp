//
// RT64
//
#ifndef RT64_MINIMAL

#include "rt64_shader_vk.h"

#include "rt64_shader_hlsli.h"
#include "rt64_shader_compiler_vk.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

// Private

#define TEXTURE_EDGE_ENABLED

enum {
	SHADER_0,
	SHADER_INPUT_1,
	SHADER_INPUT_2,
	SHADER_INPUT_3,
	SHADER_INPUT_4,
	SHADER_TEXEL0,
	SHADER_TEXEL0A,
	SHADER_TEXEL1
};

#define SHADER_OPT_ALPHA (1 << 24)
#define SHADER_OPT_TEXTURE_EDGE (1 << 26)
#define SHADER_OPT_NOISE (1 << 27)

struct ColorCombinerParams {
	int c[2][4];
	int inputCount = 0;
	bool useTextures[2] = { false, false };
	int do_single[2];
	int do_multiply[2];
	int do_mix[2];
	int color_alpha_same;
	int opt_alpha;
	int opt_texture_edge;
	int opt_noise;

	ColorCombinerParams(int shaderId) {
		for (int i = 0; i < 4; i++) {
			c[0][i] = (shaderId >> (i * 3)) & 7;
			c[1][i] = (shaderId >> (12 + i * 3)) & 7;
		}

		for (int i = 0; i < 2; i++) {
			for (int j = 0; j < 4; j++) {
				if (c[i][j] >= SHADER_INPUT_1 && c[i][j] <= SHADER_INPUT_4) {
					if (c[i][j] > inputCount) {
						inputCount = c[i][j];
					}
				}
				if (c[i][j] == SHADER_TEXEL0 || c[i][j] == SHADER_TEXEL0A) {
					useTextures[0] = true;
				}
				if (c[i][j] == SHADER_TEXEL1) {
					useTextures[1] = true;
				}
			}
		}

		do_single[0] = c[0][2] == 0;
		do_single[1] = c[1][2] == 0;
		do_multiply[0] = c[0][1] == 0 && c[0][3] == 0;
		do_multiply[1] = c[1][1] == 0 && c[1][3] == 0;
		do_mix[0] = c[0][1] == c[0][3];
		do_mix[1] = c[1][1] == c[1][3];

		color_alpha_same = (shaderId & 0xfff) == ((shaderId >> 12) & 0xfff);
		opt_alpha = (shaderId & SHADER_OPT_ALPHA) != 0;
		opt_texture_edge = (shaderId & SHADER_OPT_TEXTURE_EDGE) != 0;
		opt_noise = (shaderId & SHADER_OPT_NOISE) != 0;
	}
};

struct VertexLayout {
	int vertexSize = 0;
	int positionOffset = 0;
	int normalOffset = 0;
	int uvOffset = 0;
	int inputOffset[4] = { 0,0,0,0 };

	VertexLayout(bool vertexPosition, bool vertexNormal, bool vertexUV, int inputCount, bool useAlpha) {
		positionOffset = vertexSize; if (vertexPosition) vertexSize += 16;
		normalOffset = vertexSize; if (vertexNormal) vertexSize += 12;
		uvOffset = vertexSize; if (vertexUV) vertexSize += 8;
		for (int i = 0; i < inputCount; i++) {
			inputOffset[i] = vertexSize;
			vertexSize += useAlpha ? 16 : 12;
		}
	}
};

RT64::ShaderVK::ShaderVK(ShaderCompilerVK *compiler, unsigned int shaderId, Filter filter, AddressingMode hAddr, AddressingMode vAddr, int flags) {
	assert(compiler != nullptr);
	this->compiler = compiler;
	this->valid = true;

	bool normalMapEnabled = flags & RT64_SHADER_NORMAL_MAP_ENABLED;
	bool specularMapEnabled = flags & RT64_SHADER_SPECULAR_MAP_ENABLED;
	const std::string baseName =
		"Shader_" +
		std::to_string(shaderId) +
		"_" + std::to_string(uniqueSamplerRegisterIndex(filter, hAddr, vAddr)) +
		(normalMapEnabled ? "_Nrm" : "") +
		(specularMapEnabled ? "_Spc" : "");

	if (flags & RT64_SHADER_RASTER_ENABLED) {
		const std::string vertexShader = baseName + "VS";
		const std::string pixelShader = baseName + "PS";
		generateRasterGroup(shaderId, filter, hAddr, vAddr, vertexShader, pixelShader);
	}

	if (flags & RT64_SHADER_RAYTRACE_ENABLED) {
		const std::string hitGroup = baseName + "HitGroup";
		const std::string closestHit = baseName + "ClosestHit";
		const std::string anyHit = baseName + "AnyHit";
		const std::string shadowHitGroup = baseName + "ShadowHitGroup";
		const std::string shadowClosestHit = baseName + "ShadowClosestHit";
		const std::string shadowAnyHit = baseName + "ShadowAnyHit";
		generateSurfaceHitGroup(shaderId, filter, hAddr, vAddr, normalMapEnabled, specularMapEnabled, hitGroup, closestHit, anyHit);
		generateShadowHitGroup(shaderId, filter, hAddr, vAddr, shadowHitGroup, shadowClosestHit, shadowAnyHit);
	}

}

RT64::ShaderVK::~ShaderVK() {
	/* SPIR-V lives in std::vector, so there is nothing to release. */
}

#define SS(x) ss << x << std::endl;

unsigned int RT64::ShaderVK::uniqueSamplerRegisterIndex(Filter filter, AddressingMode hAddr, AddressingMode vAddr) {
	// Index 0 is reserved by the sampler used in the tracer.
	unsigned int uniqueID = 1;
	uniqueID += (unsigned int)(filter) * 9;
	uniqueID += (unsigned int)(hAddr) * 3;
	uniqueID += (unsigned int)(vAddr);
	return uniqueID;
}

void incMeshBuffers(std::stringstream &ss) {
	/* VULKAN PORT: D3D12 bound vertexBuffer and indexBuffer through a LOCAL
	   root signature, so each hit group's shader binding table record carried
	   the addresses of that instance's mesh. Vulkan has no local root
	   signature; its equivalent is the shader record buffer, which holds
	   per-record data in the SBT itself. The two buffer device addresses go
	   there and are read with vk::RawBufferLoad.

	   This keeps RT64's design intact — mesh buffers stay per-instance and
	   independent, so RT64_SetMesh can still reallocate a mesh freely. The
	   alternatives (descriptor-indexed arrays, or one packed buffer with
	   per-instance offsets) would have coupled unrelated meshes together. */
	SS("struct RT64MeshAddresses { uint64_t vertexAddress; uint64_t indexAddress; };");
	SS("[[vk::shader_record_ext]] ConstantBuffer<RT64MeshAddresses> gMeshAddresses;");
	SS("");
	/* Alignment 4: indices are tightly packed uint32 and vertex attributes sit
	   at combiner-derived offsets, so nothing stronger can be assumed. */
	SS("uint3 rt64LoadIndex3(uint byteOffset) {");
	SS("    uint64_t a = gMeshAddresses.indexAddress + byteOffset;");
	SS("    return uint3(vk::RawBufferLoad<uint>(a, 4), vk::RawBufferLoad<uint>(a + 4, 4), vk::RawBufferLoad<uint>(a + 8, 4));");
	SS("}");
	SS("float rt64LoadVertexFloat(uint byteOffset) {");
	SS("    return vk::RawBufferLoad<float>(gMeshAddresses.vertexAddress + byteOffset, 4);");
	SS("}");
	SS("float2 rt64LoadVertexFloat2(uint o) { return float2(rt64LoadVertexFloat(o), rt64LoadVertexFloat(o + 4)); }");
	SS("float3 rt64LoadVertexFloat3(uint o) { return float3(rt64LoadVertexFloat(o), rt64LoadVertexFloat(o + 4), rt64LoadVertexFloat(o + 8)); }");
	SS("float4 rt64LoadVertexFloat4(uint o) { return float4(rt64LoadVertexFloat(o), rt64LoadVertexFloat(o + 4), rt64LoadVertexFloat(o + 8), rt64LoadVertexFloat(o + 12)); }");
}

void getVertexData(std::stringstream &ss, bool vertexPosition, bool vertexNormal, bool vertexUV, int inputCount, bool useAlpha, bool vertexBinormalAndTangent, VertexLayout *outLayout) {
	VertexLayout vl(vertexPosition, vertexNormal, vertexUV, inputCount, useAlpha);
	/* The hit groups read the same vertex data the raster path does, so the
	   layout must be available even for a raytrace-only shader. Capturing it
	   only in generateRasterGroup left RT64_SHADER_RAYTRACE_ENABLED on its own
	   reporting a stride of zero. */
	if (outLayout != nullptr) {
		*outLayout = vl;
	}

	SS("uint3 index3 = rt64LoadIndex3((triangleIndex * 3) * 4);");

	if (vertexPosition) {
		for (int i = 0; i < 3; i++) {
			SS("float3 pos" + std::to_string(i) + " = rt64LoadVertexFloat3(index3[" + std::to_string(i) + "] * " + std::to_string(vl.vertexSize) + " + " + std::to_string(vl.positionOffset) + ");");
			SS("float3 posW" + std::to_string(i) + " = mul(instanceTransforms[instanceId].objectToWorld, float4(pos" + std::to_string(i) + ", 1.0f)).xyz; ");
		}

		SS("float3 vertexPosition = pos0 * barycentrics[0] + pos1 * barycentrics[1] + pos2 * barycentrics[2];");
	}

	if (vertexNormal) {
		for (int i = 0; i < 3; i++) {
			SS("float3 norm" + std::to_string(i) + " = rt64LoadVertexFloat3(index3[" + std::to_string(i) + "] * " + std::to_string(vl.vertexSize) + " + " + std::to_string(vl.normalOffset) + ");");
		}

		SS("float3 vertexNormal = norm0 * barycentrics[0] + norm1 * barycentrics[1] + norm2 * barycentrics[2];");
		SS("float3 triangleNormal = -cross(pos2 - pos0, pos1 - pos0);");
		SS("vertexNormal = any(vertexNormal) ? normalize(vertexNormal) : triangleNormal;");

		// Transform the triangle normal.
		SS("triangleNormal = normalize(mul(instanceTransforms[instanceId].objectToWorldNormal, float4(triangleNormal, 0.f)).xyz);");
	}

	if (vertexUV) {
		for (int i = 0; i < 3; i++) {
			SS("float2 uv" + std::to_string(i) + " = rt64LoadVertexFloat2(index3[" + std::to_string(i) + "] * " + std::to_string(vl.vertexSize) + " + " + std::to_string(vl.uvOffset) + ");");
		}

		SS("float2 vertexUV = uv0 * barycentrics[0] + uv1 * barycentrics[1] + uv2 * barycentrics[2];");
	}

	for (int i = 0; i < inputCount; i++) {
		std::string floatNum = useAlpha ? "4" : "3";
		std::string index = std::to_string(i + 1);
		for (int j = 0; j < 3; j++) {
			SS("float" + floatNum + " input" + index + std::to_string(j) + " = rt64LoadVertexFloat" + floatNum + "(index3[" + std::to_string(j) + "] * " + std::to_string(vl.vertexSize) + " + " + std::to_string(vl.inputOffset[i]) + ");");
		}

		SS("float4 input" + index + " = " + (useAlpha ? "" : "float4(") + "input" + index + "0 * barycentrics[0] + input" + index + "1 * barycentrics[1] + input" + index + "2 * barycentrics[2]" + (useAlpha ? "" : ", 1.0f)") + ";");
	}

	if (vertexBinormalAndTangent) {
		// Compute the tangent vector for the polygon.
		// Derived from http://area.autodesk.com/blogs/the-3ds-max-blog/how_the_3ds_max_scanline_renderer_computes_tangent_and_binormal_vectors_for_normal_mapping
		SS("float uva = uv1.x - uv0.x;");
		SS("float uvb = uv2.x - uv0.x;");
		SS("float uvc = uv1.y - uv0.y;");
		SS("float uvd = uv2.y - uv0.y;");
		SS("float uvk = uvb * uvc - uva * uvd;");
		SS("float3 dpos1 = pos1 - pos0;");
		SS("float3 dpos2 = pos2 - pos0;");
		SS("float3 vertexTangent;");
		SS("if (uvk != 0) vertexTangent = normalize((uvc * dpos2 - uvd * dpos1) / uvk);");
		SS("else {");
		SS("    if (uva != 0) vertexTangent = normalize(dpos1 / uva);");
		SS("    else if (uvb != 0) vertexTangent = normalize(dpos2 / uvb);");
		SS("    else vertexTangent = 0.0f;");
		SS("}");
		SS("float2 duv1 = uv1 - uv0;");
		SS("float2 duv2 = uv2 - uv1;");
		SS("duv1.y = -duv1.y;");
		SS("duv2.y = -duv2.y;");
		SS("float3 cr = cross(float3(duv1.xy, 0.0f), float3(duv2.xy, 0.0f));");
		SS("float binormalMult = (cr.z < 0.0f) ? -1.0f : 1.0f;");
		SS("float3 vertexBinormal = cross(vertexTangent, vertexNormal) * binormalMult;");
	}
}

std::string colorInput(int item, bool with_alpha, bool inputs_have_alpha, bool hint_single_element) {
	switch (item) {
	default:
	case SHADER_0:
		return with_alpha ? "float4(0.0f, 0.0f, 0.0f, 0.0f)" : "float4(0.0f, 0.0f, 0.0f, 1.0f)";
	case SHADER_INPUT_1:
		return with_alpha || !inputs_have_alpha ? "input1" : "float4(input1.rgb, 1.0f)";
	case SHADER_INPUT_2:
		return with_alpha || !inputs_have_alpha ? "input2" : "float4(input2.rgb, 1.0f)";
	case SHADER_INPUT_3:
		return with_alpha || !inputs_have_alpha ? "input3" : "float4(input3.rgb, 1.0f)";
	case SHADER_INPUT_4:
		return with_alpha || !inputs_have_alpha ? "input4" : "float4(input4.rgb, 1.0f)";
	case SHADER_TEXEL0:
		return with_alpha ? "texVal0" : "float4(texVal0.rgb, 1.0f)";
	case SHADER_TEXEL0A:
		if (hint_single_element) {
			return "float4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)";
		}
		else {
			if (with_alpha) {
				return "float4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)";
			}
			else {
				return "float4(texVal0.a, texVal0.a, texVal0.a, 1.0f)";
			}
		}
	case SHADER_TEXEL1:
		return with_alpha ? "texVal1" : "float4(texVal1.rgb, 1.0f)";
	}
}

std::string colorFormula(int c[2][4], int do_single, int do_multiply, int do_mix, bool with_alpha, int opt_alpha) {
	if (do_single) {
		return colorInput(c[0][3], with_alpha, opt_alpha, false);
	}
	else if (do_multiply) {
		return colorInput(c[0][0], with_alpha, opt_alpha, false) + " * " + colorInput(c[0][2], with_alpha, opt_alpha, true);
	}
	else if (do_mix) {
		return "lerp(" + colorInput(c[0][1], with_alpha, opt_alpha, false) + ", " + colorInput(c[0][0], with_alpha, opt_alpha, false) + ", " + colorInput(c[0][2], with_alpha, opt_alpha, true) + ")";
	}
	else {
		return "(" + colorInput(c[0][0], with_alpha, opt_alpha, false) + " - " + colorInput(c[0][1], with_alpha, opt_alpha, false) + ") * " + colorInput(c[0][2], with_alpha, opt_alpha, true) + ".r + " + colorInput(c[0][3], with_alpha, opt_alpha, false);
	}
}

std::string alphaInput(int item) {
	switch (item) {
	default:
	case SHADER_0:
		return "0.0f";
	case SHADER_INPUT_1:
		return "input1.a";
	case SHADER_INPUT_2:
		return "input2.a";
	case SHADER_INPUT_3:
		return "input3.a";
	case SHADER_INPUT_4:
		return "input4.a";
	case SHADER_TEXEL0:
		return "texVal0.a";
	case SHADER_TEXEL0A:
		return "texVal0.a";
	case SHADER_TEXEL1:
		return "texVal1.a";
	}
}

std::string alphaFormula(int c[2][4], int do_single, int do_multiply, int do_mix, bool with_alpha, int opt_alpha) {
	if (do_single) {
		return alphaInput(c[1][3]);
	}
	else if (do_multiply) {
		return alphaInput(c[1][0]) + " * " + alphaInput(c[1][2]);
	}
	else if (do_mix) {
		return "lerp(" + alphaInput(c[1][1]) + ", " + alphaInput(c[1][0]) + ", " + alphaInput(c[1][2]) + ")";
	}
	else {
		return "(" + alphaInput(c[1][0]) + " - " + alphaInput(c[1][1]) + ") * " + alphaInput(c[1][2]) + " + " + alphaInput(c[1][3]);
	}
}

void RT64::ShaderVK::generateRasterGroup(unsigned int shaderId, Filter filter, AddressingMode hAddr, AddressingMode vAddr, const std::string &vertexShaderName, const std::string &pixelShaderName) {
	ColorCombinerParams cc(shaderId);
	bool vertexUV = cc.useTextures[0] || cc.useTextures[1];
	VertexLayout vl(true, true, vertexUV, cc.inputCount, cc.opt_alpha);

	std::stringstream ss;
	SS(INCLUDE_HLSLI(MaterialsHLSLI));
	SS(INCLUDE_HLSLI(InstancesHLSLI));
	SS("int instanceId : register(b0);");

	unsigned int samplerRegisterIndex = uniqueSamplerRegisterIndex(filter, hAddr, vAddr);
	if (cc.useTextures[0]) {
		SS("SamplerState gTextureSampler : register(s" + std::to_string(samplerRegisterIndex) + ");");
		SS(INCLUDE_HLSLI(TexturesHLSLI));
	}

	// Vertex shader.
	SS("void " + vertexShaderName + "(");
	SS("    in float4 iPosition : POSITION,");
	SS("    in float3 iNormal : NORMAL,");
	if (vertexUV) {
		SS("    in float2 iUV : TEXCOORD,");
	}
	for (int i = 0; i < cc.inputCount; i++) {
		const std::string floatNumber = cc.opt_alpha ? "4" : "3";
		SS("    in float" + floatNumber + " iInput" + std::to_string(i + 1) + " : COLOR" + std::to_string(i) + ",");
	}
	SS("    out float4 oPosition : SV_POSITION,");
	SS("    out float3 oNormal : NORMAL,");
	if (vertexUV) {
		SS("    out float2 oUV : TEXCOORD" + std::string((cc.inputCount > 0) ? "," : ""));
	}
	for (int i = 0; i < cc.inputCount; i++) {
		SS("    out float4 oInput" + std::to_string(i + 1) + " : COLOR" + std::to_string(i) + std::string(((i + 1) < cc.inputCount) ? "," : ""));
	}
	SS(") {");
	SS("    oPosition = iPosition;");
	SS("    oNormal = iNormal;");
	if (vertexUV) {
		SS("    oUV = iUV;");
	}
	for (int i = 0; i < cc.inputCount; i++) {
		SS("    oInput" + std::to_string(i + 1) + " = " + std::string(cc.opt_alpha ? "" : "float4(") + "iInput" + std::to_string(i + 1) + std::string(cc.opt_alpha ? "" : ", 1.0f)") + ";");
	}
	SS("}");

	// Pixel shader.
	SS("void " + pixelShaderName + "(");
	SS("    in float4 vertexPosition : SV_POSITION,");
	SS("    in float3 vertexNormal : NORMAL,");
	if (vertexUV) {
		SS("    in float2 vertexUV : TEXCOORD,");
	}
	for (int i = 0; i < cc.inputCount; i++) {
		SS("    in float4 input" + std::to_string(i + 1) + " : COLOR" + std::to_string(i) + ",");
	}
	SS("    out float4 resultColor : SV_TARGET");
	SS(") {");

	if (cc.useTextures[0]) {
		SS("    int diffuseTexIndex = instanceMaterials[instanceId].diffuseTexIndex;");
		SS("    float4 texVal0 = gTextures[NonUniformResourceIndex(diffuseTexIndex)].Sample(gTextureSampler, vertexUV);");
	}

	if (cc.useTextures[1]) {
		// TODO
		SS("    float4 texVal1 = float4(1.0f, 0.0f, 1.0f, 1.0f);");
	}

	if (!cc.color_alpha_same && cc.opt_alpha) {
		SS("    resultColor = float4((" + colorFormula(cc.c, cc.do_single[0], cc.do_multiply[0], cc.do_mix[0], false, true) + ").rgb, " + alphaFormula(cc.c, cc.do_single[1], cc.do_multiply[1], cc.do_mix[1], true, true) + ");");
	}
	else {
		SS("    resultColor = " + colorFormula(cc.c, cc.do_single[0], cc.do_multiply[0], cc.do_mix[0], cc.opt_alpha, cc.opt_alpha) + ";");
	}
	SS("}");

	std::string shaderCode = ss.str();
	rasterGroup.pixelShaderName = pixelShaderName;
	rasterGroup.vertexShaderName = vertexShaderName;
	compileShaderCode(shaderCode, pixelShaderName, "ps_6_3", rasterGroup.spirvPS);
	compileShaderCode(shaderCode, vertexShaderName, "vs_6_3", rasterGroup.spirvVS);
	
	/* VULKAN PORT: the original built a D3D12 input layout, blend state and
	   pipeline state object here. In Vulkan those belong to pipeline creation,
	   not shader generation, so the vertex layout is captured as portable data
	   for the pipeline builder to translate. Discarding it would lose the
	   offsets, which are derived from the colour combiner and not recoverable
	   later. */
	recordVertexStride((uint32_t)vl.vertexSize);
	rasterGroup.attributes.clear();
	rasterGroup.attributes.push_back({ VertexAttribute::Position,
		(uint32_t)vl.positionOffset, 4 });
	rasterGroup.attributes.push_back({ VertexAttribute::Normal,
		(uint32_t)vl.normalOffset, 3 });
	if (vertexUV) {
		rasterGroup.attributes.push_back({ VertexAttribute::TexCoord,
			(uint32_t)vl.uvOffset, 2 });
	}
	for (int i = 0; i < cc.inputCount; i++) {
		rasterGroup.attributes.push_back({ VertexAttribute::Color,
			(uint32_t)vl.inputOffset[i], cc.opt_alpha ? 4u : 3u });
	}
	rasterGroup.vertexStride = (uint32_t)vl.vertexSize;
	rasterGroup.alphaBlend = true;
}

void RT64::ShaderVK::generateSurfaceHitGroup(unsigned int shaderId, Filter filter, AddressingMode hAddr, AddressingMode vAddr, bool normalMapEnabled, bool specularMapEnabled, const std::string &hitGroupName, const std::string &closestHitName, const std::string &anyHitName) {
	VertexLayout capturedLayout(true, true, false, 0, false);
	ColorCombinerParams cc(shaderId);

	std::stringstream ss;
	incMeshBuffers(ss);

	SS(INCLUDE_HLSLI(MaterialsHLSLI));
	SS(INCLUDE_HLSLI(InstancesHLSLI));
	SS(INCLUDE_HLSLI(GlobalHitBuffersHLSLI));
	SS(INCLUDE_HLSLI(RayHLSLI));
	SS(INCLUDE_HLSLI(RandomHLSLI));
	SS(INCLUDE_HLSLI(GlobalParamsHLSLI));

	unsigned int samplerRegisterIndex = uniqueSamplerRegisterIndex(filter, hAddr, vAddr);
	if (cc.useTextures[0]) {
		SS("SamplerState gTextureSampler : register(s" + std::to_string(samplerRegisterIndex) + ");");
		SS(INCLUDE_HLSLI(TexturesHLSLI));
	}

	SS("[shader(\"anyhit\")]");
	SS("void " << anyHitName << "(inout HitInfo payload, Attributes attrib) {");
	SS("    uint instanceId = InstanceIndex();");
	SS("    uint triangleIndex = PrimitiveIndex();");
	SS("    float3 barycentrics = float3((1.0f - attrib.bary.x - attrib.bary.y), attrib.bary.x, attrib.bary.y);");
	SS("    float4 diffuseColorMix = instanceMaterials[instanceId].diffuseColorMix;");

	bool vertexUV = cc.useTextures[0] || cc.useTextures[1];
	getVertexData(ss, true, true, vertexUV, cc.inputCount, cc.opt_alpha, vertexUV && normalMapEnabled, &capturedLayout);

	if (cc.useTextures[0]) {
		SS("	float2 ddx, ddy;");
		SS("	RayDiff propRayDiff = propagateRayDiffs(payload.rayDiff, WorldRayDirection(), RayTCurrent(), triangleNormal);");
		SS("	float2 dBarydx, dBarydy;");
		SS("	computeBarycentricDifferentials(propRayDiff, WorldRayDirection(), posW1 - posW0, posW2 - posW0, triangleNormal, dBarydx, dBarydy);");
		SS("	computeTextureDifferentials(dBarydx, dBarydy, uv0, uv1, uv2, ddx, ddy);");
		SS("    int diffuseTexIndex = instanceMaterials[instanceId].diffuseTexIndex;");
		SS("    float4 texVal0 = gTextures[NonUniformResourceIndex(diffuseTexIndex)].SampleGrad(gTextureSampler, vertexUV, ddx, ddy);");
		SS("    texVal0.rgb = lerp(texVal0.rgb, diffuseColorMix.rgb, max(-diffuseColorMix.a, 0.0f));");
	}

	if (cc.useTextures[1]) {
		// TODO
		SS("    float4 texVal1 = float4(1.0f, 0.0f, 1.0f, 1.0f);");
	}

	if (!cc.color_alpha_same && cc.opt_alpha) {
		SS("    float4 resultColor = float4((" + colorFormula(cc.c, cc.do_single[0], cc.do_multiply[0], cc.do_mix[0], false, true) + ").rgb, " + alphaFormula(cc.c, cc.do_single[1], cc.do_multiply[1], cc.do_mix[1], true, true) + ");");
	}
	else {
		SS("    float4 resultColor = " + colorFormula(cc.c, cc.do_single[0], cc.do_multiply[0], cc.do_mix[0], cc.opt_alpha, cc.opt_alpha) + ";");
	}

	// Only mix the final diffuse color if the alpha is positive.
	SS("    resultColor.rgb = lerp(resultColor.rgb, diffuseColorMix.rgb, max(diffuseColorMix.a, 0.0f));");

	// Apply the solid alpha multiplier.
	SS("    resultColor.a = clamp(instanceMaterials[instanceId].solidAlphaMultiplier * resultColor.a, 0.0f, 1.0f);");

#ifdef TEXTURE_EDGE_ENABLED
	if (cc.opt_texture_edge) {
		SS("    if (resultColor.a > 0.3f) {");
		SS("      resultColor.a = 1.0f;");
		SS("    }");
		SS("    else {");
		SS("      IgnoreHit();");
		SS("    }");
	}
#endif

	if (cc.opt_noise) {
		SS("    uint seed = initRand(DispatchRaysIndex().x + DispatchRaysIndex().y * DispatchRaysDimensions().x, frameCount, 16);");
		SS("    resultColor.a *= round(nextRand(seed));");
	}
	
	SS("vertexNormal = normalize(mul(instanceTransforms[instanceId].objectToWorldNormal, float4(vertexNormal, 0.f)).xyz);");
	SS("float normalSign = (dot(triangleNormal, WorldRayDirection()) <= 0.0f) ? 1.0f : -1.0f;");
	SS("vertexNormal *= normalSign;");
	
	if (vertexUV && normalMapEnabled) {
		SS("    vertexTangent = normalize(mul(instanceTransforms[instanceId].objectToWorldNormal, float4(vertexTangent, 0.f)).xyz) * normalSign;");
		SS("    vertexBinormal = normalize(mul(instanceTransforms[instanceId].objectToWorldNormal, float4(vertexBinormal, 0.f)).xyz) * normalSign;");
		SS("    int normalTexIndex = instanceMaterials[instanceId].normalTexIndex;");
		SS("    if (normalTexIndex >= 0) {");
		SS("        float uvDetailScale = instanceMaterials[instanceId].uvDetailScale;");
		SS("        float3 normalColor = gTextures[NonUniformResourceIndex(normalTexIndex)].SampleGrad(gTextureSampler, vertexUV * uvDetailScale, ddx * uvDetailScale, ddy * uvDetailScale).xyz;");
		SS("        normalColor = (normalColor * 2.0f) - 1.0f;");
		SS("        float3 newNormal = normalize(vertexNormal * normalColor.z + vertexTangent * normalColor.x + vertexBinormal * normalColor.y);");
		SS("        vertexNormal = newNormal;");
		SS("    }");
	}

	SS("	float3 prevWorldPos = mul(instanceTransforms[instanceId].objectToWorldPrevious, float4(vertexPosition, 1.0f));");
	SS("	float3 curWorldPos = mul(instanceTransforms[instanceId].objectToWorld, float4(vertexPosition, 1.0f));");
	SS("	float3 vertexFlow = curWorldPos - prevWorldPos;");
	SS("    float3 vertexSpecular = float3(1.0f, 1.0f, 1.0f);");
	if (vertexUV && specularMapEnabled) {
		SS("    int specularTexIndex = instanceMaterials[instanceId].specularTexIndex;");
		SS("    if (specularTexIndex >= 0) {");
		SS("        float uvDetailScale = instanceMaterials[instanceId].uvDetailScale;");
		SS("        vertexSpecular = gTextures[NonUniformResourceIndex(specularTexIndex)].SampleGrad(gTextureSampler, vertexUV * uvDetailScale, ddx * uvDetailScale, ddy * uvDetailScale).rgb;");
		SS("    }");
	}

	SS("    uint2 pixelIdx = DispatchRaysIndex().xy;");
	SS("    uint2 pixelDims = DispatchRaysDimensions().xy;");
	SS("    uint hitStride = pixelDims.x * pixelDims.y;");

	// HACK: Add some bias for the comparison based on the instance ID so coplanar surfaces are friendlier with each other.
	// This can likely be implemented as an instance property at some point to control depth sorting.
	SS("    float tval = WithDistanceBias(RayTCurrent(), instanceId);");
	SS("    uint hi = getHitBufferIndex(min(payload.nhits, MAX_HIT_QUERIES), pixelIdx, pixelDims);");
	SS("    uint minHi = getHitBufferIndex(0, pixelIdx, pixelDims);");
	SS("    uint lo = hi - hitStride;");
	SS("    while ((hi > minHi) && (tval < gHitDistAndFlow[lo].x)) {");
	SS("        gHitDistAndFlow[hi] = gHitDistAndFlow[lo];");
	SS("        gHitColor[hi] = gHitColor[lo];");
	SS("        gHitNormal[hi] = gHitNormal[lo];");
	SS("        gHitSpecular[hi] = gHitSpecular[lo];");
	SS("        gHitInstanceId[hi] = gHitInstanceId[lo];");
	SS("        hi -= hitStride;");
	SS("        lo -= hitStride;");
	SS("    }");
	SS("    uint hitPos = hi / hitStride;");
	SS("    if (hitPos < MAX_HIT_QUERIES) {");
	SS("        gHitDistAndFlow[hi] = float4(tval, vertexFlow);");
	SS("        gHitColor[hi] = resultColor;");
	SS("        gHitNormal[hi] = float4(vertexNormal, 1.0f);");
	SS("        gHitSpecular[hi] = float4(vertexSpecular, 1.0f);");
	SS("        gHitInstanceId[hi] = instanceId;");
	SS("        ++payload.nhits;");
	SS("        if (hitPos != MAX_HIT_QUERIES - 1) {");
	SS("            IgnoreHit();");
	SS("        }");
	SS("    }");
	SS("    else {");
	SS("        IgnoreHit();");
	SS("    }");
	SS("}");
	SS("[shader(\"closesthit\")]");
	SS("void " << closestHitName << "(inout HitInfo payload, Attributes attrib) { }");

	// Compile shader.
	std::string shaderCode = ss.str();
#ifdef RT64_DUMP_GENERATED
	if (const char *path = std::getenv("RT64_DUMP_GENERATED")) {
		FILE *f = fopen(path, "w");
		if (f) { fwrite(shaderCode.data(), 1, shaderCode.size(), f); fclose(f); }
	}
#endif
	recordVertexStride((uint32_t)capturedLayout.vertexSize);
	compileShaderCode(shaderCode, "", "lib_6_3", surfaceHitGroup.spirv);
	surfaceHitGroup.hitGroupName = hitGroupName;
	surfaceHitGroup.closestHitName = closestHitName;
	surfaceHitGroup.anyHitName = anyHitName;
}

void RT64::ShaderVK::generateShadowHitGroup(unsigned int shaderId, Filter filter, AddressingMode hAddr, AddressingMode vAddr, const std::string &hitGroupName, const std::string &closestHitName, const std::string &anyHitName) {
	VertexLayout capturedLayout(true, true, false, 0, false);
	ColorCombinerParams cc(shaderId);
	std::stringstream ss;
	incMeshBuffers(ss);
	
	SS(INCLUDE_HLSLI(MaterialsHLSLI));
	SS(INCLUDE_HLSLI(InstancesHLSLI));
	SS(INCLUDE_HLSLI(RayHLSLI));
	SS(INCLUDE_HLSLI(RandomHLSLI));
	SS(INCLUDE_HLSLI(GlobalParamsHLSLI));

	unsigned int samplerRegisterIndex = uniqueSamplerRegisterIndex(filter, hAddr, vAddr);
	if (cc.useTextures[0]) {
		SS("SamplerState gTextureSampler : register(s" + std::to_string(samplerRegisterIndex) + ");");
		SS(INCLUDE_HLSLI(TexturesHLSLI));
	}
	
	SS("[shader(\"anyhit\")]");
	SS("void " << anyHitName << "(inout ShadowHitInfo payload, Attributes attrib) {");
	if (cc.opt_alpha) {
		SS("    uint instanceId = InstanceIndex();");
		SS("    uint triangleIndex = PrimitiveIndex();");
		SS("    float3 barycentrics = float3((1.0f - attrib.bary.x - attrib.bary.y), attrib.bary.x, attrib.bary.y);");

		getVertexData(ss, true, true, cc.useTextures[0] || cc.useTextures[1], cc.inputCount, cc.opt_alpha, false, &capturedLayout);

		if (cc.useTextures[0]) {
			SS("    int diffuseTexIndex = instanceMaterials[instanceId].diffuseTexIndex;");
			SS("    float4 texVal0 = gTextures[NonUniformResourceIndex(diffuseTexIndex)].SampleLevel(gTextureSampler, vertexUV, 0);");
		}

		if (cc.useTextures[1]) {
			// TODO
			SS("    float4 texVal1 = float4(1.0f, 0.0f, 1.0f, 1.0f);");
		}

		if (!cc.color_alpha_same && cc.opt_alpha) {
			SS("    float resultAlpha = " + alphaFormula(cc.c, cc.do_single[1], cc.do_multiply[1], cc.do_mix[1], true, true) + ";");
		}
		else {
			SS("    float resultAlpha = (" + colorFormula(cc.c, cc.do_single[0], cc.do_multiply[0], cc.do_mix[0], cc.opt_alpha, cc.opt_alpha) + ").a;");
		}

		SS("    resultAlpha = clamp(resultAlpha * instanceMaterials[instanceId].shadowAlphaMultiplier, 0.0f, 1.0f);");

#ifdef TEXTURE_EDGE_ENABLED
		if (cc.opt_texture_edge) {
			SS("    if (resultAlpha > 0.3f) {");
			SS("      resultAlpha = 1.0f;");
			SS("    }");
			SS("    else {");
			SS("      IgnoreHit();");
			SS("    }");
		}
#endif

		if (cc.opt_noise) {
			SS("    uint seed = initRand(DispatchRaysIndex().x + DispatchRaysIndex().y * DispatchRaysDimensions().x, frameCount, 16);");
			SS("    resultAlpha *= round(nextRand(seed));");
		}

		SS("    payload.shadowHit = max(payload.shadowHit - resultAlpha, 0.0f);");
		SS("    if (payload.shadowHit > 0.0f) {");
		SS("		IgnoreHit();");
		SS("    }");
	}
	else {
		SS("payload.shadowHit = 0.0f;");
	}
	SS("}");
	SS("[shader(\"closesthit\")]");
	SS("void " << closestHitName << "(inout ShadowHitInfo payload, Attributes attrib) { }");

	// Compile shader.
	std::string shaderCode = ss.str();
	recordVertexStride((uint32_t)capturedLayout.vertexSize);
	compileShaderCode(shaderCode, "", "lib_6_3", shadowHitGroup.spirv);
	shadowHitGroup.hitGroupName = hitGroupName;
	shadowHitGroup.closestHitName = closestHitName;
	shadowHitGroup.anyHitName = anyHitName;
}

void RT64::ShaderVK::recordVertexStride(uint32_t stride) {
	/* First writer wins: the raster and hit paths compute the same layout for
	   a given combiner, so whichever runs first is authoritative. */
	if (rasterGroup.vertexStride == 0) {
		rasterGroup.vertexStride = stride;
	}
}

void RT64::ShaderVK::compileShaderCode(const std::string &shaderCode,
	const std::string &entryName, const std::string &profile,
	std::vector<uint32_t> &spirv)
{
	std::string error;
	if (!compiler->compile(shaderCode, entryName, profile, spirv, error)) {
		/* Record rather than throw: one bad material must not take down the
		   renderer, and the generated source is what needs inspecting. */
		lastError = error;
		valid = false;
		spirv.clear();
	}
}

const RT64::ShaderVK::RasterGroup &RT64::ShaderVK::getRasterGroup() const {
	return rasterGroup;
}

RT64::ShaderVK::HitGroup &RT64::ShaderVK::getSurfaceHitGroup() {
	return surfaceHitGroup;
}

RT64::ShaderVK::HitGroup &RT64::ShaderVK::getShadowHitGroup() {
	return shadowHitGroup;
}

bool RT64::ShaderVK::hasRasterGroup() const {
	return !rasterGroup.spirvPS.empty() || !rasterGroup.spirvVS.empty();
}

bool RT64::ShaderVK::hasHitGroups() const {
	return !surfaceHitGroup.spirv.empty() || !shadowHitGroup.spirv.empty();
}

// Public

RT64::ShaderVK::Filter RT64::convertFilter(unsigned int filter) {
	switch (filter) {
	case RT64_SHADER_FILTER_LINEAR:
		return RT64::ShaderVK::Filter::Linear;
	case RT64_SHADER_FILTER_POINT:
	default:
		return RT64::ShaderVK::Filter::Point;
	}
}

RT64::ShaderVK::AddressingMode RT64::convertAddressingMode(unsigned int mode) {
	switch (mode) {
	case RT64_SHADER_ADDRESSING_CLAMP:
		return RT64::ShaderVK::AddressingMode::Clamp;
	case RT64_SHADER_ADDRESSING_MIRROR:
		return RT64::ShaderVK::AddressingMode::Mirror;
	case RT64_SHADER_ADDRESSING_WRAP:
	default:
		return RT64::ShaderVK::AddressingMode::Wrap;
	}
}

/* RT64_CreateShader / RT64_DestroyShader are exported from
   rt64_stub.cpp alongside the rest of the C ABI. */

#endif