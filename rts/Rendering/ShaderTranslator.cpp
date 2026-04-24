/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Rendering/ShaderTranslator.h"

#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <spirv_msl.hpp>

#include <mutex>

namespace Shader {

namespace {

// glslang ships a 'glslang-default-resource-limits' helper lib that exposes
// GetDefaultResources(). We intentionally opted out of glslang's binaries when
// fetching (no tests, no CLI, no installer), which also drops that helper. The
// struct below is a straight copy of the library's built-in defaults, copied
// here so we stay a single-file dependency consumer and do not have to flip on
// more of glslang's CMake. Values cribbed from DefaultTBuiltInResource in
// glslang/StandAlone/ResourceLimits.cpp.
TBuiltInResource DefaultResources()
{
	TBuiltInResource r{};
	r.maxLights = 32;
	r.maxClipPlanes = 6;
	r.maxTextureUnits = 32;
	r.maxTextureCoords = 32;
	r.maxVertexAttribs = 64;
	r.maxVertexUniformComponents = 4096;
	r.maxVaryingFloats = 64;
	r.maxVertexTextureImageUnits = 32;
	r.maxCombinedTextureImageUnits = 80;
	r.maxTextureImageUnits = 32;
	r.maxFragmentUniformComponents = 4096;
	r.maxDrawBuffers = 32;
	r.maxVertexUniformVectors = 128;
	r.maxVaryingVectors = 8;
	r.maxFragmentUniformVectors = 16;
	r.maxVertexOutputVectors = 16;
	r.maxFragmentInputVectors = 15;
	r.minProgramTexelOffset = -8;
	r.maxProgramTexelOffset = 7;
	r.maxClipDistances = 8;
	r.maxComputeWorkGroupCountX = 65535;
	r.maxComputeWorkGroupCountY = 65535;
	r.maxComputeWorkGroupCountZ = 65535;
	r.maxComputeWorkGroupSizeX = 1024;
	r.maxComputeWorkGroupSizeY = 1024;
	r.maxComputeWorkGroupSizeZ = 64;
	r.maxComputeUniformComponents = 1024;
	r.maxComputeTextureImageUnits = 16;
	r.maxComputeImageUniforms = 8;
	r.maxComputeAtomicCounters = 8;
	r.maxComputeAtomicCounterBuffers = 1;
	r.maxVaryingComponents = 60;
	r.maxVertexOutputComponents = 64;
	r.maxGeometryInputComponents = 64;
	r.maxGeometryOutputComponents = 128;
	r.maxFragmentInputComponents = 128;
	r.maxImageUnits = 8;
	r.maxCombinedImageUnitsAndFragmentOutputs = 8;
	r.maxCombinedShaderOutputResources = 8;
	r.maxImageSamples = 0;
	r.maxVertexImageUniforms = 0;
	r.maxTessControlImageUniforms = 0;
	r.maxTessEvaluationImageUniforms = 0;
	r.maxGeometryImageUniforms = 0;
	r.maxFragmentImageUniforms = 8;
	r.maxCombinedImageUniforms = 8;
	r.maxGeometryTextureImageUnits = 16;
	r.maxGeometryOutputVertices = 256;
	r.maxGeometryTotalOutputComponents = 1024;
	r.maxGeometryUniformComponents = 1024;
	r.maxGeometryVaryingComponents = 64;
	r.maxTessControlInputComponents = 128;
	r.maxTessControlOutputComponents = 128;
	r.maxTessControlTextureImageUnits = 16;
	r.maxTessControlUniformComponents = 1024;
	r.maxTessControlTotalOutputComponents = 4096;
	r.maxTessEvaluationInputComponents = 128;
	r.maxTessEvaluationOutputComponents = 128;
	r.maxTessEvaluationTextureImageUnits = 16;
	r.maxTessEvaluationUniformComponents = 1024;
	r.maxTessPatchComponents = 120;
	r.maxPatchVertices = 32;
	r.maxTessGenLevel = 64;
	r.maxViewports = 16;
	r.maxVertexAtomicCounters = 0;
	r.maxTessControlAtomicCounters = 0;
	r.maxTessEvaluationAtomicCounters = 0;
	r.maxGeometryAtomicCounters = 0;
	r.maxFragmentAtomicCounters = 8;
	r.maxCombinedAtomicCounters = 8;
	r.maxAtomicCounterBindings = 1;
	r.maxVertexAtomicCounterBuffers = 0;
	r.maxTessControlAtomicCounterBuffers = 0;
	r.maxTessEvaluationAtomicCounterBuffers = 0;
	r.maxGeometryAtomicCounterBuffers = 0;
	r.maxFragmentAtomicCounterBuffers = 1;
	r.maxCombinedAtomicCounterBuffers = 1;
	r.maxAtomicCounterBufferSize = 16384;
	r.maxTransformFeedbackBuffers = 4;
	r.maxTransformFeedbackInterleavedComponents = 64;
	r.maxCullDistances = 8;
	r.maxCombinedClipAndCullDistances = 8;
	r.maxSamples = 4;
	r.maxMeshOutputVerticesNV = 256;
	r.maxMeshOutputPrimitivesNV = 512;
	r.maxMeshWorkGroupSizeX_NV = 32;
	r.maxMeshWorkGroupSizeY_NV = 1;
	r.maxMeshWorkGroupSizeZ_NV = 1;
	r.maxTaskWorkGroupSizeX_NV = 32;
	r.maxTaskWorkGroupSizeY_NV = 1;
	r.maxTaskWorkGroupSizeZ_NV = 1;
	r.maxMeshViewCountNV = 4;

	r.limits.nonInductiveForLoops = 1;
	r.limits.whileLoops = 1;
	r.limits.doWhileLoops = 1;
	r.limits.generalUniformIndexing = 1;
	r.limits.generalAttributeMatrixVectorIndexing = 1;
	r.limits.generalVaryingIndexing = 1;
	r.limits.generalSamplerIndexing = 1;
	r.limits.generalVariableIndexing = 1;
	r.limits.generalConstantMatrixVectorIndexing = 1;
	return r;
}

// glslang requires a one-shot global init. We wrap that in a std::call_once so
// the translator can be invoked from any thread without leaking global state on
// shutdown. Finalize() is intentionally not called; glslang's teardown is racy
// with static destructors in dependents, and we only pay the cost once.
void EnsureGlslangInit()
{
	static std::once_flag once;
	std::call_once(once, []() { glslang::InitializeProcess(); });
}

EShLanguage ToGlslangStage(Stage stage)
{
	switch (stage) {
		case Stage::Vertex:   return EShLangVertex;
		case Stage::Fragment: return EShLangFragment;
	}
	return EShLangVertex;
}

spv::ExecutionModel ToSpvExecutionModel(Stage stage)
{
	switch (stage) {
		case Stage::Vertex:   return spv::ExecutionModelVertex;
		case Stage::Fragment: return spv::ExecutionModelFragment;
	}
	return spv::ExecutionModelVertex;
}

bool CompileGlslToSpirv(Stage stage, const std::string& source, const std::string& entry,
                       std::vector<uint32_t>& spirvOut, std::string& logOut)
{
	EnsureGlslangInit();

	const EShLanguage glslStage = ToGlslangStage(stage);
	glslang::TShader shader(glslStage);

	const char* src = source.c_str();
	shader.setStrings(&src, 1);
	shader.setEntryPoint(entry.c_str());
	shader.setSourceEntryPoint(entry.c_str());
	shader.setEnvInput(glslang::EShSourceGlsl, glslStage, glslang::EShClientVulkan, 100);
	shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
	shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

	static const TBuiltInResource resources = DefaultResources();
	const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

	if (!shader.parse(&resources, 450, false, messages)) {
		logOut += shader.getInfoLog();
		logOut += shader.getInfoDebugLog();
		return false;
	}

	glslang::TProgram program;
	program.addShader(&shader);
	if (!program.link(messages)) {
		logOut += program.getInfoLog();
		logOut += program.getInfoDebugLog();
		return false;
	}

	glslang::GlslangToSpv(*program.getIntermediate(glslStage), spirvOut);
	return !spirvOut.empty();
}

TranslateResult CrossCompileToMsl(Stage stage, const std::vector<uint32_t>& spirv, const std::string& entry)
{
	TranslateResult result;
	try {
		spirv_cross::CompilerMSL compiler(spirv);

		spirv_cross::CompilerMSL::Options opts;
		opts.platform = spirv_cross::CompilerMSL::Options::macOS;
		opts.set_msl_version(2, 3);
		compiler.set_msl_options(opts);

		// Pin the entry point name so downstream MTLLibrary lookup is stable.
		compiler.set_entry_point(entry, ToSpvExecutionModel(stage));

		// Preserve GLSL binding= numbers as MSL slot numbers across all
		// resource types. By default spirv-cross renumbers MSL slots 0..N
		// per-type, so e.g. a sampler2D with GLSL binding=1 lands on
		// [[texture(0)]] [[sampler(0)]], which then mismatches callers
		// that BindTexture(/*slot=*/1, ...) from engine code. Explicit
		// MSLResourceBinding keeps GLSL and Metal bindings aligned.
		{
			const spv::ExecutionModel execModel = ToSpvExecutionModel(stage);
			const spirv_cross::ShaderResources res = compiler.get_shader_resources();

			auto pin = [&](const spirv_cross::Resource& r, bool hasBuffer, bool hasTexture, bool hasSampler) {
				spirv_cross::MSLResourceBinding rb{};
				rb.stage       = execModel;
				rb.desc_set    = compiler.get_decoration(r.id, spv::DecorationDescriptorSet);
				rb.binding     = compiler.get_decoration(r.id, spv::DecorationBinding);
				rb.msl_buffer  = hasBuffer  ? rb.binding : 0;
				rb.msl_texture = hasTexture ? rb.binding : 0;
				rb.msl_sampler = hasSampler ? rb.binding : 0;
				compiler.add_msl_resource_binding(rb);
			};

			for (const auto& r : res.uniform_buffers)   pin(r, /*buf*/true,  /*tex*/false, /*smp*/false);
			for (const auto& r : res.storage_buffers)   pin(r, /*buf*/true,  /*tex*/false, /*smp*/false);
			for (const auto& r : res.push_constant_buffers) pin(r, /*buf*/true, false, false);
			for (const auto& r : res.sampled_images)    pin(r, /*buf*/false, /*tex*/true,  /*smp*/true);   // combined sampler2D
			for (const auto& r : res.separate_images)   pin(r, /*buf*/false, /*tex*/true,  /*smp*/false);
			for (const auto& r : res.separate_samplers) pin(r, /*buf*/false, /*tex*/false, /*smp*/true);
			for (const auto& r : res.storage_images)    pin(r, /*buf*/false, /*tex*/true,  /*smp*/false);
		}

		result.msl = compiler.compile();
		result.ok  = !result.msl.empty();
		if (!result.ok)
			result.log = "spirv-cross produced empty MSL";

		// DBG: dump MSL to stderr so we can see which slots spirv-cross
		// picked for textures / samplers / buffers. Volume is manageable -
		// only called when pipelines are compiled (at init / first draw).
		if (result.ok && getenv("RECOIL_DUMP_MSL") != nullptr) {
			fprintf(stderr, "[ShaderTranslator] MSL stage=%d entry=%s\n%s\n===\n",
				static_cast<int>(stage), entry.c_str(), result.msl.c_str());
			fflush(stderr);
		}
	} catch (const std::exception& e) {
		result.ok = false;
		result.log = std::string("spirv-cross: ") + e.what();
	}
	return result;
}

} // namespace

TranslateResult TranslateGlslToMsl(Stage stage, const std::string& source, const std::string& entry)
{
	TranslateResult result;
	std::vector<uint32_t> spirv;
	if (!CompileGlslToSpirv(stage, source, entry, spirv, result.log)) {
		result.ok = false;
		return result;
	}
	return CrossCompileToMsl(stage, spirv, entry);
}

TranslateResult TranslateSpirvToMsl(Stage stage, const std::vector<uint32_t>& spirv, const std::string& entry)
{
	return CrossCompileToMsl(stage, spirv, entry);
}

} // namespace Shader
