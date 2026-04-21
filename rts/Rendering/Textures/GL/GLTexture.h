#pragma once

#include <memory>

#include "Rendering/Textures/ISampler.h"
#include "Rendering/Textures/ITexture.h"
#include "Rendering/Textures/Texture.hpp"

class GLTexture final : public ITexture
{
public:
	~GLTexture() override;

	explicit GLTexture(std::unique_ptr<GL::TextureBase>&& texture_);
	GLTexture(uint32_t texTarget, uint32_t textureId, const int2& size, uint32_t internalFormat, int32_t numLevels, uint32_t numPages = 1, bool ownTextureId = true);

	bool IsValid() const override;
	uint32_t GetNativeId() const override;
	uint32_t GetTarget() const override;
	uint32_t GetInternalFormat() const override;
	uint32_t GetNumPages() const override;
	int32_t GetNumLevels() const override;
	int2 GetSize() const override;

	void Bind() override;
	void Bind(uint32_t relSlot) override;
	void Unbind() override;
	void Unbind(uint32_t relSlot) override;

	void UploadImage(const void* data, uint32_t layer = 0, int level = 0) override;
	void UploadSubImage(const void* data, int xOffset, int yOffset, int width, int height, uint32_t layer = 0, int level = 0) override;
	void GenerateMipmaps() override;
	uint32_t DisOwn() override;

	GL::TextureBase& GetTexture() { return *texture; }
	const GL::TextureBase& GetTexture() const { return *texture; }

private:
	template <typename T>
	T* GetAs() { return dynamic_cast<T*>(texture.get()); }

	template <typename T>
	const T* GetAs() const { return dynamic_cast<const T*>(texture.get()); }

private:
	std::unique_ptr<GL::TextureBase> texture;
	uint32_t rawTextureId = 0;
	uint32_t texTarget = 0;
	uint32_t internalFormat = 0;
	uint32_t numPages = 1;
	int32_t numLevels = 0;
	int2 size;
	bool ownRawTextureId = false;
};

class GLSampler final : public ISampler
{
public:
	explicit GLSampler(GL::TextureCreationParams params_);

	void Apply(uint32_t texTarget) const override;
	const GL::TextureCreationParams& GetParams() const override { return params; }

private:
	GL::TextureCreationParams params;
};

std::unique_ptr<ITexture> CreateGLTexture2D(const int2& size, uint32_t internalFormat, const GL::TextureCreationParams& params, bool wantCompress = true);
std::unique_ptr<ITexture> CreateGLTexture2DArray(const int2& size, uint32_t numPages, uint32_t internalFormat, const GL::TextureCreationParams& params, bool wantCompress = true);
std::unique_ptr<ITexture> CreateGLImportedTexture(uint32_t texTarget, uint32_t textureId, const int2& size, uint32_t internalFormat, int32_t numLevels, uint32_t numPages = 1, bool takeOwnership = true);
std::unique_ptr<ISampler> CreateGLSampler(const GL::TextureCreationParams& params);
