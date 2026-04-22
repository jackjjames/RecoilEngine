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
};
