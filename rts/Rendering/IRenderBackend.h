/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>
#include <memory>

#include "Rendering/Platform/IRenderContext.h"
#include "Rendering/Platform/IPresenter.h"
#include "System/type2.h"

class IRenderTarget;
class ITexture;
class ISampler;

namespace GL {
	struct TextureCreationParams;
}

class IRenderBackend
{
public:
	virtual ~IRenderBackend() = default;

	virtual IRenderContext& GetRenderContext() = 0;
	virtual const IRenderContext& GetRenderContext() const = 0;
	virtual IPresenter& GetPresenter() = 0;
	virtual const IPresenter& GetPresenter() const = 0;
	virtual std::unique_ptr<IRenderTarget> CreateRenderTarget() const = 0;
	virtual std::unique_ptr<ITexture> CreateTexture2D(const int2& size, uint32_t internalFormat, const GL::TextureCreationParams& params, bool wantCompress = true) const = 0;
	virtual std::unique_ptr<ITexture> CreateTexture2DArray(const int2& size, uint32_t numPages, uint32_t internalFormat, const GL::TextureCreationParams& params, bool wantCompress = true) const = 0;
	virtual std::unique_ptr<ITexture> CreateImportedTexture(uint32_t texTarget, uint32_t textureId, const int2& size, uint32_t internalFormat, int32_t numLevels, uint32_t numPages = 1, bool takeOwnership = true) const = 0;
	virtual std::unique_ptr<ISampler> CreateSampler(const GL::TextureCreationParams& params) const = 0;
};
