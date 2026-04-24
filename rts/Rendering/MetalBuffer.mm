/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Rendering/IBuffer.h"
#include "Rendering/MetalRenderGlobals.h"
#include "Rendering/MetalResources.h"

#import <Metal/Metal.h>

#include <cstring>
#include <memory>

namespace {

class MetalBuffer final : public IBuffer
{
public:
	explicit MetalBuffer(size_t size, const void* initialData)
		: sizeBytes(size)
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)MetalGlobals::GetDevice();
		if (device == nil || size == 0)
			return;

		// MTLResourceStorageModeShared lets CPU and GPU access the same
		// backing pages on Apple Silicon without explicit sync. Good enough
		// for the stage-5 uniform path; a managed/private path can come
		// later once we are doing large static VBOs.
		MTLResourceOptions options = MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache;
		if (initialData != nullptr) {
			buffer = [device newBufferWithBytes:initialData length:size options:options];
		} else {
			buffer = [device newBufferWithLength:size options:options];
		}
	}

	~MetalBuffer() override
	{
		buffer = nil;
	}

	bool IsValid() const override { return buffer != nil; }
	uint32_t GetNativeId() const override { return 0; }
	size_t GetSize() const override { return sizeBytes; }

	void UpdateData(const void* data, size_t size, size_t offset) override
	{
		if (buffer == nil || data == nullptr)
			return;
		if (offset + size > sizeBytes)
			return;
		auto* dst = static_cast<uint8_t*>([buffer contents]) + offset;
		std::memcpy(dst, data, size);
	}

	void BindUniformRange(uint32_t slot, size_t offset, size_t size) const override
	{
		if (buffer == nil)
			return;

		MetalGlobals::BufferBinding binding;
		binding.mtlBuffer = (__bridge void*)buffer;
		binding.offset    = offset;
		binding.size      = size == 0 ? sizeBytes : size;
		MetalGlobals::SetUniformBinding(slot, binding);
	}

	// Metal has a single MTLBuffer argument-table per stage; SSBO and UBO are
	// the same object from the encoder's POV. Forward to the uniform path.
	void BindStorageRange(uint32_t slot, size_t offset, size_t size) const override
	{
		BindUniformRange(slot, offset, size);
	}

	// Indirect-draw buffer is passed directly to [encoder drawPrimitives:
	// indirectBuffer:] at draw time in MetalShaderPipeline; there is no
	// separate "indirect" bind slot like GL has. This is a no-op -- callers
	// hand the IBuffer to the draw method instead.
	void BindIndirect() const override {}

	void* GetMtlBuffer() const { return (__bridge void*)buffer; }

private:
	id<MTLBuffer> buffer = nil;
	size_t sizeBytes = 0;
};

} // namespace

std::unique_ptr<IBuffer> CreateMetalBuffer(size_t size, const void* initialData)
{
	return std::make_unique<MetalBuffer>(size, initialData);
}

namespace MetalResources {

void* GetMtlBuffer(const IBuffer& buffer)
{
	// dynamic_cast is safe here: the Metal build only ever instantiates
	// MetalBuffer behind IBuffer. Returns nullptr for any other subtype so
	// callers can fall back gracefully.
	if (const auto* mb = dynamic_cast<const MetalBuffer*>(&buffer))
		return mb->GetMtlBuffer();
	return nullptr;
}

} // namespace MetalResources
