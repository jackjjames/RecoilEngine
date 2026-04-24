#pragma once

#include <cstddef>
#include <cstdint>

class IBuffer
{
public:
	virtual ~IBuffer() = default;

	virtual bool IsValid() const = 0;
	virtual uint32_t GetNativeId() const = 0;
	virtual size_t GetSize() const = 0;
	virtual void UpdateData(const void* data, size_t size, size_t offset = 0) = 0;
	virtual void BindUniformRange(uint32_t slot, size_t offset, size_t size) const = 0;

	// Shader-storage (SSBO) bind. Default forwards to BindUniformRange because
	// on Metal both UBO and SSBO resolve to the same MTLBuffer argument slot;
	// the GL implementation overrides to target GL_SHADER_STORAGE_BUFFER.
	virtual void BindStorageRange(uint32_t slot, size_t offset, size_t size) const
	{
		BindUniformRange(slot, offset, size);
	}

	// Indirect-draw bind. Default is a no-op; backends override when they
	// support multi-draw-indirect (GL: glBindBuffer(GL_DRAW_INDIRECT_BUFFER);
	// Metal: passed directly to [encoder drawPrimitives:indirectBuffer:] ).
	virtual void BindIndirect() const {}
};
