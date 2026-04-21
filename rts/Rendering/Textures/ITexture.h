#pragma once

#include <cstdint>

#include "System/type2.h"

class ITexture
{
public:
	virtual ~ITexture() = default;

	virtual bool IsValid() const = 0;
	virtual uint32_t GetNativeId() const = 0;
	virtual uint32_t GetTarget() const = 0;
	virtual uint32_t GetInternalFormat() const = 0;
	virtual uint32_t GetNumPages() const = 0;
	virtual int32_t GetNumLevels() const = 0;
	virtual int2 GetSize() const = 0;

	virtual void Bind() = 0;
	virtual void Bind(uint32_t relSlot) = 0;
	virtual void Unbind() = 0;
	virtual void Unbind(uint32_t relSlot) = 0;

	virtual void UploadImage(const void* data, uint32_t layer = 0, int level = 0) = 0;
	virtual void UploadSubImage(const void* data, int xOffset, int yOffset, int width, int height, uint32_t layer = 0, int level = 0) = 0;
	virtual void GenerateMipmaps() = 0;
	virtual uint32_t DisOwn() = 0;
};
