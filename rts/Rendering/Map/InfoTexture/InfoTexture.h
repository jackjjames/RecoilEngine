/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include "Rendering/GL/myGL.h"
#include "Rendering/Textures/ITexture.h"
#include "Rendering/Textures/Texture.hpp"
#include "System/type2.h"
#include <memory>
#include <string>

class CInfoTexture
{
public:
	CInfoTexture();
	CInfoTexture(const std::string& name, GL::Texture2D&& texture, int2 texSize);
	virtual ~CInfoTexture() {}

public:
	virtual GLuint GetTexture() { return texture.GetId(); }
	virtual ITexture& GetTextureHandle() const;
	int2 GetTexSize()     const { return texSize; }
	const std::string& GetName() const { return name; }
protected:
	friend class IInfoTextureHandler;

	GL::Texture2D texture;
	mutable std::unique_ptr<ITexture> textureHandle;
	mutable uint32_t textureHandleId = 0;
	std::string name;
	int2 texSize;
};

class CDummyInfoTexture: public CInfoTexture {
public:
	CDummyInfoTexture() : CInfoTexture("dummy", {}, int2(0, 0)) {}
};