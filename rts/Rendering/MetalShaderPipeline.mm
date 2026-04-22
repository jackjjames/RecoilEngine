/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Rendering/IBuffer.h"
#include "Rendering/MetalResources.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Rendering/Textures/ITexture.h"

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

MTLVertexFormat ToMtlVertexFormat(VertexFormat fmt)
{
	switch (fmt) {
		case VertexFormat::Float1: return MTLVertexFormatFloat;
		case VertexFormat::Float2: return MTLVertexFormatFloat2;
		case VertexFormat::Float3: return MTLVertexFormatFloat3;
		case VertexFormat::Float4: return MTLVertexFormatFloat4;
	}
	return MTLVertexFormatFloat4;
}

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

		// Build an MTLVertexDescriptor when the caller described a vertex
		// layout. Empty layout (no attributes + no bindings) leaves
		// pipelineDesc.vertexDescriptor as nil, which is the right thing
		// for vertex-less procedural passes (see TrianglePass).
		if (!desc.vertexAttributes.empty()) {
			MTLVertexDescriptor* vtxDesc = [MTLVertexDescriptor new];

			for (const auto& attr : desc.vertexAttributes) {
				const uint32_t mtlBufferIdx = desc.metalVertexBufferBaseSlot + attr.bufferSlot;
				vtxDesc.attributes[attr.location].format      = ToMtlVertexFormat(attr.format);
				vtxDesc.attributes[attr.location].offset      = attr.offset;
				vtxDesc.attributes[attr.location].bufferIndex = mtlBufferIdx;
			}
			for (const auto& binding : desc.vertexBindings) {
				const uint32_t mtlBufferIdx = desc.metalVertexBufferBaseSlot + binding.slot;
				vtxDesc.layouts[mtlBufferIdx].stride       = binding.stride;
				vtxDesc.layouts[mtlBufferIdx].stepRate     = 1;
				vtxDesc.layouts[mtlBufferIdx].stepFunction = MTLVertexStepFunctionPerVertex;
			}

			pipelineDesc.vertexDescriptor = vtxDesc;
		}

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

	void BindVertexBuffer(uint32_t slot, const IBuffer& buffer, size_t offset) override
	{
		void* mtlBuffer = MetalResources::GetMtlBuffer(buffer);
		if (mtlBuffer == nullptr)
			return;

		MetalGlobals::BufferBinding binding;
		binding.mtlBuffer = mtlBuffer;
		binding.offset    = offset;
		binding.size      = buffer.GetSize() - offset;
		MetalGlobals::SetVertexBufferBinding(slot, binding);
	}

	void BindTexture(uint32_t slot, ITexture& texture) override
	{
		MetalGlobals::TextureBinding binding;
		binding.mtlTexture = MetalResources::GetMtlTexture(texture);
		binding.mtlSampler = MetalResources::GetMtlSampler(texture);
		MetalGlobals::SetTextureBinding(slot, binding);
	}

	void Draw(PrimitiveTopology topology, uint32_t firstVertex, uint32_t vertexCount) override
	{
		if (!valid || vertexCount == 0)
			return;

		auto encoder = (__bridge id<MTLRenderCommandEncoder>)MetalGlobals::GetCurrentEncoder();
		if (encoder == nil)
			return;

		ApplyBindings(encoder);

		[encoder drawPrimitives:ToMtlPrimitive(topology) vertexStart:firstVertex vertexCount:vertexCount];
	}

	void DrawIndexed(PrimitiveTopology topology, uint32_t indexCount, IndexType indexType,
	                 const IBuffer& indexBuffer, size_t indexOffset) override
	{
		if (!valid || indexCount == 0)
			return;

		auto encoder = (__bridge id<MTLRenderCommandEncoder>)MetalGlobals::GetCurrentEncoder();
		if (encoder == nil)
			return;

		void* mtlIndexBufferRaw = MetalResources::GetMtlBuffer(indexBuffer);
		if (mtlIndexBufferRaw == nullptr)
			return;
		auto mtlIndexBuffer = (__bridge id<MTLBuffer>)mtlIndexBufferRaw;

		ApplyBindings(encoder);

		const MTLIndexType mtlIndexType = (indexType == IndexType::Uint16)
			? MTLIndexTypeUInt16
			: MTLIndexTypeUInt32;

		[encoder drawIndexedPrimitives:ToMtlPrimitive(topology)
		                    indexCount:indexCount
		                     indexType:mtlIndexType
		                   indexBuffer:mtlIndexBuffer
		             indexBufferOffset:indexOffset];
	}

private:
	static MTLPrimitiveType ToMtlPrimitive(PrimitiveTopology topology)
	{
		switch (topology) {
			case PrimitiveTopology::Triangles:     return MTLPrimitiveTypeTriangle;
			case PrimitiveTopology::TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
			case PrimitiveTopology::Lines:         return MTLPrimitiveTypeLine;
			case PrimitiveTopology::LineStrip:     return MTLPrimitiveTypeLineStrip;
			case PrimitiveTopology::Points:        return MTLPrimitiveTypePoint;
		}
		return MTLPrimitiveTypeTriangle;
	}

	// Apply pipeline state + the pending per-draw binding tables onto the
	// encoder. Uniform buffers replay onto both vertex + fragment slots
	// (spirv-cross maps Vulkan descriptor set 0 bindings onto identical MTL
	// buffer indices). Vertex buffers go onto metalVertexBufferBaseSlot + slot
	// to match the MTLVertexDescriptor layout assignments. Sampled textures
	// bind onto the fragment stage only (current callers are all fragment
	// samplers; vertex-stage samplers can land with the unit drawer).
	void ApplyBindings(id<MTLRenderCommandEncoder> encoder)
	{
		[encoder setRenderPipelineState:pipelineState];

		for (uint32_t slot = 0; slot < MetalGlobals::kMaxBindSlots; ++slot) {
			const auto& binding = MetalGlobals::GetUniformBinding(slot);
			if (binding.mtlBuffer == nullptr)
				continue;
			auto buf = (__bridge id<MTLBuffer>)binding.mtlBuffer;
			[encoder setVertexBuffer:buf   offset:binding.offset atIndex:slot];
			[encoder setFragmentBuffer:buf offset:binding.offset atIndex:slot];
		}

		for (uint32_t slot = 0; slot < MetalGlobals::kMaxBindSlots; ++slot) {
			const auto& binding = MetalGlobals::GetVertexBufferBinding(slot);
			if (binding.mtlBuffer == nullptr)
				continue;
			auto buf = (__bridge id<MTLBuffer>)binding.mtlBuffer;
			const uint32_t mtlSlot = desc.metalVertexBufferBaseSlot + slot;
			[encoder setVertexBuffer:buf offset:binding.offset atIndex:mtlSlot];
		}

		for (uint32_t slot = 0; slot < MetalGlobals::kMaxBindSlots; ++slot) {
			const auto& binding = MetalGlobals::GetTextureBinding(slot);
			if (binding.mtlTexture == nullptr)
				continue;
			auto tex = (__bridge id<MTLTexture>)binding.mtlTexture;
			[encoder setFragmentTexture:tex atIndex:slot];
			if (binding.mtlSampler != nullptr) {
				auto samp = (__bridge id<MTLSamplerState>)binding.mtlSampler;
				[encoder setFragmentSamplerState:samp atIndex:slot];
			}
		}
	}

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
