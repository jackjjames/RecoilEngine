/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Metal ITexture implementation. Backs ITexture with an id<MTLTexture> and
// a cached id<MTLSamplerState> built from TextureCreationParams. Scope is
// intentionally narrow: 2D single-level textures used by the loading-screen
// path (bitmap quad). Mipmaps, 2D arrays, compressed formats, and texture
// importing are stubbed or TODO'd.

#include "Rendering/MetalRenderGlobals.h"
#include "Rendering/MetalResources.h"
#include "Rendering/Textures/ISampler.h"
#include "Rendering/Textures/ITexture.h"
#include "Rendering/Textures/TextureCreationParams.hpp"

#include "System/Log/ILog.h"
#include "System/type2.h"

#import <Metal/Metal.h>

#include <cstdint>
#include <cstring>
#include <memory>

// GL internalFormat constants we map. We do not include the GL headers on
// Metal builds; duplicate the handful of constants we care about. These are
// stable (set in stone by the GL spec) so duplicating is safe.
namespace {
constexpr uint32_t kGL_R8     = 0x8229;
constexpr uint32_t kGL_RG8    = 0x822B;
constexpr uint32_t kGL_RGB8   = 0x8051;
constexpr uint32_t kGL_RGBA8  = 0x8058;
constexpr uint32_t kGL_SRGB8  = 0x8C41;
constexpr uint32_t kGL_SRGB8_ALPHA8 = 0x8C43;
constexpr uint32_t kGL_TEXTURE_2D = 0x0DE1;

struct MtlFormatInfo {
	MTLPixelFormat fmt  = MTLPixelFormatInvalid;
	uint32_t       bpp  = 0; // bytes per pixel
};

MtlFormatInfo MapGlInternalFormat(uint32_t internalFormat)
{
	switch (internalFormat) {
		case kGL_R8:            return { MTLPixelFormatR8Unorm,      1 };
		case kGL_RG8:           return { MTLPixelFormatRG8Unorm,     2 };
		case kGL_RGBA8:         return { MTLPixelFormatRGBA8Unorm,   4 };
		case kGL_SRGB8_ALPHA8:  return { MTLPixelFormatRGBA8Unorm_sRGB, 4 };
		// RGB8 has no native Metal counterpart; upload path pads to RGBA.
		case kGL_RGB8:          return { MTLPixelFormatRGBA8Unorm,   4 };
		case kGL_SRGB8:         return { MTLPixelFormatRGBA8Unorm_sRGB, 4 };
	}
	return { MTLPixelFormatInvalid, 0 };
}

MTLSamplerMinMagFilter ChooseMinFilter(const GL::TextureCreationParams& params, int32_t numLevels)
{
	(void)numLevels;
	return params.linearTextureFilter ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
}

MTLSamplerMinMagFilter ChooseMagFilter(const GL::TextureCreationParams& params)
{
	return params.linearTextureFilter ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
}

MTLSamplerMipFilter ChooseMipFilter(const GL::TextureCreationParams& params)
{
	return params.linearMipMapFilter ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
}

id<MTLSamplerState> BuildSampler(id<MTLDevice> device, const GL::TextureCreationParams& params, int32_t numLevels)
{
	MTLSamplerDescriptor* desc = [MTLSamplerDescriptor new];
	desc.minFilter = ChooseMinFilter(params, numLevels);
	desc.magFilter = ChooseMagFilter(params);
	desc.mipFilter = (numLevels > 1) ? ChooseMipFilter(params) : MTLSamplerMipFilterNotMipmapped;
	// TextureCreationParams still carries wrap mode as optional GL enums;
	// for the loading-screen bitmap quad clamp-to-edge is the sensible
	// default. Wider wrap-mode translation can land when the real
	// TextureCreationParams-driven consumers (unit / map atlases) show up.
	desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
	desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
	desc.rAddressMode = MTLSamplerAddressModeClampToEdge;
	desc.maxAnisotropy = params.aniso > 1.0f ? static_cast<NSUInteger>(params.aniso) : 1;
	return [device newSamplerStateWithDescriptor:desc];
}

class MetalTexture final : public ITexture
{
public:
	MetalTexture(const int2& size_, uint32_t internalFormat_, const GL::TextureCreationParams& params, int32_t numLevels_)
		: size(size_)
		, internalFormat(internalFormat_)
		, numLevels(numLevels_)
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)MetalGlobals::GetDevice();
		if (device == nil || size.x <= 0 || size.y <= 0)
			return;

		formatInfo = MapGlInternalFormat(internalFormat);
		if (formatInfo.fmt == MTLPixelFormatInvalid) {
			LOG_L(L_WARNING, "[MetalTexture] unsupported internalFormat 0x%x; creating RGBA8Unorm fallback", internalFormat);
			formatInfo = { MTLPixelFormatRGBA8Unorm, 4 };
		}

		MTLTextureDescriptor* texDesc = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:formatInfo.fmt
			                         width:static_cast<NSUInteger>(size.x)
			                        height:static_cast<NSUInteger>(size.y)
			                     mipmapped:(numLevels > 1)];
		texDesc.usage = MTLTextureUsageShaderRead;
		texDesc.storageMode = MTLStorageModeShared;
		if (numLevels > 1)
			texDesc.mipmapLevelCount = numLevels;

		texture = [device newTextureWithDescriptor:texDesc];
		sampler = BuildSampler(device, params, numLevels);
	}

	~MetalTexture() override
	{
		texture = nil;
		sampler = nil;
	}

	bool IsValid() const override { return texture != nil; }
	uint32_t GetNativeId() const override { return 0; }
	uint32_t GetTarget() const override { return kGL_TEXTURE_2D; }
	uint32_t GetInternalFormat() const override { return internalFormat; }
	uint32_t GetNumPages() const override { return 1; }
	int32_t GetNumLevels() const override { return numLevels; }
	int2 GetSize() const override { return size; }

	// ITexture::Bind() is defined in GL terms (it pokes the binding onto
	// the current GL texture unit). On Metal the binding is deferred to
	// MetalShaderPipeline::BindTexture -> MetalGlobals, which is what
	// IShaderPipeline callers should use. Keep these as no-ops so legacy
	// call sites (post-shim) compile, but actual binding goes through the
	// pipeline.
	void Bind() override {}
	void Bind(uint32_t /*relSlot*/) override {}
	void Unbind() override {}
	void Unbind(uint32_t /*relSlot*/) override {}

	void UploadImage(const void* data, uint32_t /*layer*/ = 0, int level = 0) override
	{
		if (texture == nil || data == nullptr)
			return;
		const uint32_t mipW = std::max(1, size.x >> level);
		const uint32_t mipH = std::max(1, size.y >> level);

		// RGB8 internalFormat gets padded to RGBA8Unorm inside CreateTexture
		// (Metal has no 3-byte format); for now fail loudly - real consumers
		// should upload the RGBA counterpart. The loading-screen bitmap path
		// is RGBA already.
		if (internalFormat == kGL_RGB8 || internalFormat == kGL_SRGB8) {
			LOG_L(L_WARNING, "[MetalTexture] RGB8 upload without RGBA padding is not implemented; skipping");
			return;
		}

		const NSUInteger bytesPerRow = mipW * formatInfo.bpp;
		MTLRegion region = MTLRegionMake2D(0, 0, mipW, mipH);
		[texture replaceRegion:region
		           mipmapLevel:level
		             withBytes:data
		           bytesPerRow:bytesPerRow];
	}

	void UploadSubImage(const void* data, int xOffset, int yOffset, int width, int height, uint32_t /*layer*/ = 0, int level = 0) override
	{
		if (texture == nil || data == nullptr || width <= 0 || height <= 0)
			return;
		if (internalFormat == kGL_RGB8 || internalFormat == kGL_SRGB8) {
			LOG_L(L_WARNING, "[MetalTexture] RGB8 sub-image upload without RGBA padding is not implemented; skipping");
			return;
		}
		const NSUInteger bytesPerRow = width * formatInfo.bpp;
		MTLRegion region = MTLRegionMake2D(xOffset, yOffset, width, height);
		[texture replaceRegion:region
		           mipmapLevel:level
		             withBytes:data
		           bytesPerRow:bytesPerRow];
	}

	void GenerateMipmaps() override
	{
		// Needs a MTLBlitCommandEncoder on the currently-active command
		// buffer to call generateMipmapsForTexture:. Skip for now - callers
		// on the loading-screen path only upload single-level bitmaps. Will
		// wire through MetalFrame helpers when the texture consumers that
		// actually need mipmaps (map textures, unit textures) show up.
	}

	uint32_t DisOwn() override { return 0; }

	void* GetMtlTexture() const { return (__bridge void*)texture; }
	void* GetMtlSampler() const { return (__bridge void*)sampler; }

private:
	int2 size;
	uint32_t internalFormat = 0;
	int32_t numLevels = 1;
	MtlFormatInfo formatInfo{};

	id<MTLTexture> texture = nil;
	id<MTLSamplerState> sampler = nil;
};

class MetalSampler final : public ISampler
{
public:
	explicit MetalSampler(const GL::TextureCreationParams& params_)
		: params(params_)
	{}

	void Apply(uint32_t /*texTarget*/) const override {}
	const GL::TextureCreationParams& GetParams() const override { return params; }

private:
	GL::TextureCreationParams params;
};

} // namespace

namespace MetalResources {

void* GetMtlTexture(const ITexture& texture)
{
	if (const auto* mt = dynamic_cast<const MetalTexture*>(&texture))
		return mt->GetMtlTexture();
	return nullptr;
}

void* GetMtlSampler(const ITexture& texture)
{
	if (const auto* mt = dynamic_cast<const MetalTexture*>(&texture))
		return mt->GetMtlSampler();
	return nullptr;
}

} // namespace MetalResources

std::unique_ptr<ITexture> CreateMetalTexture2D(const int2& size, uint32_t internalFormat, const GL::TextureCreationParams& params)
{
	return std::make_unique<MetalTexture>(size, internalFormat, params, params.reqNumLevels > 0 ? params.reqNumLevels : 1);
}

std::unique_ptr<ISampler> CreateMetalSampler(const GL::TextureCreationParams& params)
{
	return std::make_unique<MetalSampler>(params);
}
