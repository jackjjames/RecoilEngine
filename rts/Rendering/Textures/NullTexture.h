#pragma once

#include "Rendering/Textures/ITexture.h"

namespace {

class NullTexture final : public ITexture
{
public:
	bool IsValid() const override { return false; }
	uint32_t GetNativeId() const override { return 0; }
	uint32_t GetTarget() const override { return 0; }
	uint32_t GetInternalFormat() const override { return 0; }
	uint32_t GetNumPages() const override { return 0; }
	int32_t GetNumLevels() const override { return 0; }
	int2 GetSize() const override { return {0, 0}; }

	void Bind() override {}
	void Bind(uint32_t) override {}
	void Unbind() override {}
	void Unbind(uint32_t) override {}

	void UploadImage(const void*, uint32_t, int) override {}
	void UploadSubImage(const void*, int, int, int, int, uint32_t, int) override {}
	void GenerateMipmaps() override {}
	uint32_t DisOwn() override { return 0; }
};

} // namespace

inline ITexture& GetNullTexture()
{
	static NullTexture nullTexture;
	return nullTexture;
}
