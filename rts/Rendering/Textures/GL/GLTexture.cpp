#include "Rendering/Textures/GL/GLTexture.h"

#include <array>
#include <memory>

#include "Rendering/GL/myGL.h"

namespace {

void ApplyTextureParams(const GL::TextureCreationParams& params, uint32_t texTarget, int32_t numLevels)
{
	const auto minFilter = params.GetMinFilter(numLevels);
	const auto magFilter = params.GetMagFilter();

	glTexParameteri(texTarget, GL_TEXTURE_MAG_FILTER, magFilter);
	glTexParameteri(texTarget, GL_TEXTURE_MIN_FILTER, minFilter);

	if (params.wrapModes.has_value()) {
		static constexpr std::array<uint32_t, 3> texWrapModes{
			GL_TEXTURE_WRAP_S,
			GL_TEXTURE_WRAP_T,
			GL_TEXTURE_WRAP_R
		};

		const auto& wrapModes = params.wrapModes.value();
		for (size_t i = 0; auto wrapMode : wrapModes) {
			glTexParameteri(texTarget, texWrapModes[i++], wrapMode);
		}
	} else {
		const auto texWrapMode = params.GetWrapMode();

		glTexParameteri(texTarget, GL_TEXTURE_WRAP_S, texWrapMode);
		glTexParameteri(texTarget, GL_TEXTURE_WRAP_T, texWrapMode);
		glTexParameteri(texTarget, GL_TEXTURE_WRAP_R, texWrapMode);
	}

	if (params.clampBorder.has_value())
		glTexParameterfv(texTarget, GL_TEXTURE_BORDER_COLOR, &params.clampBorder.value().x);

	if (params.lodBias != 0.0f)
		glTexParameterf(texTarget, GL_TEXTURE_LOD_BIAS, params.lodBias);

	if (params.aniso > 0.0f)
		glTexParameterf(texTarget, GL_TEXTURE_MAX_ANISOTROPY, params.aniso);
}

} // namespace

GLTexture::GLTexture(std::unique_ptr<GL::TextureBase>&& texture_)
	: texture(std::move(texture_))
{
}

bool GLTexture::IsValid() const
{
	return texture && texture->IsValid();
}

uint32_t GLTexture::GetNativeId() const
{
	return texture ? texture->GetId() : 0;
}

uint32_t GLTexture::GetTarget() const
{
	return texture ? texture->GetTarget() : 0;
}

uint32_t GLTexture::GetInternalFormat() const
{
	return texture ? texture->GetInternalFormat() : 0;
}

uint32_t GLTexture::GetNumPages() const
{
	if (const auto* texArray = GetAs<GL::Texture2DArray>())
		return texArray->GetNumPages();

	return 1u;
}

int32_t GLTexture::GetNumLevels() const
{
	return texture ? texture->GetNumLevels() : 0;
}

int2 GLTexture::GetSize() const
{
	if (const auto* tex2D = GetAs<GL::Texture2D>())
		return tex2D->GetSize();

	if (const auto* texArray = GetAs<GL::Texture2DArray>())
		return texArray->GetSize();

	return int2();
}

void GLTexture::Bind()
{
	texture->Bind();
}

void GLTexture::Bind(uint32_t relSlot)
{
	texture->Bind(relSlot);
}

void GLTexture::Unbind()
{
	texture->Unbind();
}

void GLTexture::Unbind(uint32_t relSlot)
{
	texture->Unbind(relSlot);
}

void GLTexture::UploadImage(const void* data, uint32_t layer, int level)
{
	if (auto* tex2D = GetAs<GL::Texture2D>()) {
		tex2D->UploadImage(data, level);
		return;
	}

	if (auto* texArray = GetAs<GL::Texture2DArray>()) {
		texArray->UploadImage(data, layer, level);
		return;
	}

	assert(false);
}

void GLTexture::UploadSubImage(const void* data, int xOffset, int yOffset, int width, int height, uint32_t layer, int level)
{
	if (auto* tex2D = GetAs<GL::Texture2D>()) {
		tex2D->UploadSubImage(data, xOffset, yOffset, width, height, level);
		return;
	}

	if (auto* texArray = GetAs<GL::Texture2DArray>()) {
		texArray->UploadSubImage(data, layer, xOffset, yOffset, width, height, level);
		return;
	}

	assert(false);
}

void GLTexture::GenerateMipmaps()
{
	texture->ProduceMipmaps();
}

uint32_t GLTexture::DisOwn()
{
	return texture ? texture->DisOwn() : 0u;
}

GLSampler::GLSampler(GL::TextureCreationParams params_)
	: params(std::move(params_))
{
}

void GLSampler::Apply(uint32_t texTarget) const
{
	ApplyTextureParams(params, texTarget, params.reqNumLevels);
}

std::unique_ptr<ITexture> CreateGLTexture2D(const int2& size, uint32_t internalFormat, const GL::TextureCreationParams& params, bool wantCompress)
{
	return std::make_unique<GLTexture>(std::make_unique<GL::Texture2D>(size, internalFormat, params, wantCompress));
}

std::unique_ptr<ITexture> CreateGLTexture2DArray(const int2& size, uint32_t numPages, uint32_t internalFormat, const GL::TextureCreationParams& params, bool wantCompress)
{
	return std::make_unique<GLTexture>(std::make_unique<GL::Texture2DArray>(size, numPages, internalFormat, params, wantCompress));
}

std::unique_ptr<ISampler> CreateGLSampler(const GL::TextureCreationParams& params)
{
	return std::make_unique<GLSampler>(params);
}
