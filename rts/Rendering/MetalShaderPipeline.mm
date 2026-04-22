/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Rendering/Shaders/IShaderPipeline.h"

#include "Rendering/MetalRenderGlobals.h"
#include "Rendering/ShaderTranslator.h"
#include "System/Log/ILog.h"

#import <Metal/Metal.h>

#include <memory>
#include <string>

namespace {

// spirv-cross renames the entry point to main0 to avoid clashing with the C
// runtime. Both stages get that name; we mint fresh MTLFunction handles from
// the per-stage MTLLibrary.
static NSString* const kEntryPointName = @"main0";

bool CompileStage(id<MTLDevice> device, Shader::Stage stage, const std::string& glsl,
                  id<MTLLibrary>* libOut, id<MTLFunction>* fnOut, std::string& log)
{
	auto result = Shader::TranslateGlslToMsl(stage, glsl);
	if (!result.ok) {
		log += result.log;
		return false;
	}

	NSString* src = [NSString stringWithUTF8String:result.msl.c_str()];

	MTLCompileOptions* opts = [MTLCompileOptions new];
	opts.languageVersion = MTLLanguageVersion2_3;

	NSError* err = nil;
	id<MTLLibrary> lib = [device newLibraryWithSource:src options:opts error:&err];
	if (lib == nil) {
		log += err.localizedDescription.UTF8String ? err.localizedDescription.UTF8String : "MTLLibrary compile failed";
		log += '\n';
		return false;
	}

	id<MTLFunction> fn = [lib newFunctionWithName:kEntryPointName];
	if (fn == nil) {
		log += "MSL entry point main0 not found";
		log += '\n';
		return false;
	}

	*libOut = lib;
	*fnOut  = fn;
	return true;
}

class MetalShaderPipeline final : public IShaderPipeline
{
public:
	explicit MetalShaderPipeline(PipelineDesc desc_)
		: desc(std::move(desc_))
	{}

	~MetalShaderPipeline() override
	{
		Release();
	}

	void BindAttribLocation(const std::string&, uint32_t) override {}
	void BindOutputLocation(const std::string&, uint32_t) override {}

	void Enable() override
	{
		MetalGlobals::SetCurrentPipelineState(IsValid() ? (__bridge void*)pipelineState : nullptr);
	}

	void Disable() override
	{
		if (MetalGlobals::GetCurrentPipelineState() == (__bridge void*)pipelineState)
			MetalGlobals::SetCurrentPipelineState(nullptr);
	}

	void Link() override
	{
		log.clear();
		valid = false;

		id<MTLDevice> device = (__bridge id<MTLDevice>)MetalGlobals::GetDevice();
		if (device == nil) {
			log = "Metal device not initialized";
			return;
		}

		if (!CompileStage(device, Shader::Stage::Vertex, desc.vertexSource, &vertexLib, &vertexFn, log))
			return;
		if (!CompileStage(device, Shader::Stage::Fragment, desc.fragmentSource, &fragmentLib, &fragmentFn, log))
			return;

		MTLRenderPipelineDescriptor* pipelineDesc = [MTLRenderPipelineDescriptor new];
		pipelineDesc.label = [NSString stringWithUTF8String:desc.name.c_str()];
		pipelineDesc.vertexFunction = vertexFn;
		pipelineDesc.fragmentFunction = fragmentFn;
		// BGRA8Unorm matches the CAMetalLayer pixelFormat set in
		// MetalRenderContext::CreateContext. When the backbuffer format
		// diverges from the layer (offscreen RT etc.), callers will need
		// to describe that through PipelineDesc.
		pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

		NSError* err = nil;
		pipelineState = [device newRenderPipelineStateWithDescriptor:pipelineDesc error:&err];
		if (pipelineState == nil) {
			log += err.localizedDescription.UTF8String ? err.localizedDescription.UTF8String : "pipeline state creation failed";
			log += '\n';
			return;
		}

		valid = true;
	}

	bool Validate() override { return valid; }

	void Release() override
	{
		if (valid || pipelineState != nil) {
			if (MetalGlobals::GetCurrentPipelineState() == (__bridge void*)pipelineState)
				MetalGlobals::SetCurrentPipelineState(nullptr);
		}
		pipelineState = nil;
		vertexFn = nil;
		fragmentFn = nil;
		vertexLib = nil;
		fragmentLib = nil;
		valid = false;
	}

	void Reload(bool, bool validate) override
	{
		Release();
		Link();
		if (validate && !valid)
			LOG_L(L_WARNING, "[MetalShaderPipeline] reload failed for %s: %s", desc.name.c_str(), log.c_str());
	}

	void AttachShaderObject(Shader::IShaderObject*) override {}
	const std::string& GetName() const override { return desc.name; }
	const std::string& GetLog() const override { return log; }
	bool IsValid() const override { return valid; }

	void Draw(PrimitiveTopology topology, uint32_t firstVertex, uint32_t vertexCount) override
	{
		if (!valid || vertexCount == 0)
			return;

		auto encoder = (__bridge id<MTLRenderCommandEncoder>)MetalGlobals::GetCurrentEncoder();
		if (encoder == nil)
			return;

		[encoder setRenderPipelineState:pipelineState];

		// Replay the pending uniform-buffer table onto both the vertex and
		// fragment argument slots. spirv-cross maps Vulkan descriptor set 0
		// bindings onto identical MTL buffer indices, so slot N goes to
		// buffer(N) on both stages.
		for (uint32_t slot = 0; slot < MetalGlobals::kMaxBindSlots; ++slot) {
			const auto& binding = MetalGlobals::GetUniformBinding(slot);
			if (binding.mtlBuffer == nullptr)
				continue;
			auto buf = (__bridge id<MTLBuffer>)binding.mtlBuffer;
			[encoder setVertexBuffer:buf   offset:binding.offset atIndex:slot];
			[encoder setFragmentBuffer:buf offset:binding.offset atIndex:slot];
		}

		MTLPrimitiveType primType = MTLPrimitiveTypeTriangle;
		switch (topology) {
			case PrimitiveTopology::Triangles:     primType = MTLPrimitiveTypeTriangle; break;
			case PrimitiveTopology::TriangleStrip: primType = MTLPrimitiveTypeTriangleStrip; break;
			case PrimitiveTopology::Lines:         primType = MTLPrimitiveTypeLine; break;
			case PrimitiveTopology::LineStrip:     primType = MTLPrimitiveTypeLineStrip; break;
			case PrimitiveTopology::Points:        primType = MTLPrimitiveTypePoint; break;
		}

		[encoder drawPrimitives:primType vertexStart:firstVertex vertexCount:vertexCount];
	}

private:
	PipelineDesc desc;
	std::string log;
	bool valid = false;

	id<MTLLibrary> vertexLib = nil;
	id<MTLLibrary> fragmentLib = nil;
	id<MTLFunction> vertexFn = nil;
	id<MTLFunction> fragmentFn = nil;
	id<MTLRenderPipelineState> pipelineState = nil;
};

} // namespace

std::unique_ptr<IShaderPipeline> CreateMetalShaderPipeline(const PipelineDesc& desc)
{
	auto pipeline = std::make_unique<MetalShaderPipeline>(desc);
	pipeline->Link();
	if (!pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalShaderPipeline] %s did not link: %s", desc.name.c_str(), pipeline->GetLog().c_str());
	}
	return pipeline;
}
