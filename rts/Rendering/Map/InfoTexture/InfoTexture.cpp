/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "InfoTexture.h"

#include "Rendering/GlobalRendering.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/Textures/NullTexture.h"


CInfoTexture::CInfoTexture()
	: texture{}
	, texSize(0, 0)
{}

CInfoTexture::CInfoTexture(const std::string& _name, GL::Texture2D&& _texture, int2 _texSize)
	: texture(std::move(_texture))
	, name(_name)
	, texSize(_texSize)
{}

ITexture& CInfoTexture::GetTextureHandle() const
{
	if (!texture.IsValid())
		return GetNullTexture();
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return GetNullTexture();

	const uint32_t textureId = texture.GetId();

	if (textureHandle == nullptr || textureHandleId != textureId) {
		textureHandle = globalRendering->renderBackend->CreateImportedTexture(
			texture.GetTarget(),
			textureId,
			texture.GetSize(),
			texture.GetInternalFormat(),
			texture.GetNumLevels(),
			1,
			false
		);
		textureHandleId = textureId;
	}

	return (textureHandle != nullptr) ? *textureHandle : GetNullTexture();
}