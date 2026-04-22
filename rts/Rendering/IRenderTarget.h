/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include "Rendering/GL/myGL.h"
#include "System/float4.h"
#include "System/type2.h"

struct RenderTargetBlendState
{
	bool enabled = false;
	GLenum srcColor = GL_ONE;
	GLenum dstColor = GL_ZERO;
	GLenum srcAlpha = GL_ONE;
	GLenum dstAlpha = GL_ZERO;
};

struct RenderTargetDepthState
{
	bool testEnabled = true;
	bool writeEnabled = true;
	GLenum func = GL_LEQUAL;
};

class IRenderTarget
{
public:
	virtual ~IRenderTarget() = default;

	virtual void Bind() = 0;
	virtual void Unbind() = 0;
	virtual void ClearColor(const float4& color) = 0;
	virtual void ClearDepth(float depth) = 0;
	virtual void SetBlendState(const RenderTargetBlendState& state) = 0;
	virtual void SetDepthState(const RenderTargetDepthState& state) = 0;
	virtual void AttachNativeColor(unsigned int idx) = 0;
	virtual void AttachTexture(GLenum attachment, GLuint texId, GLuint texTarget, int mipLevel = 0, int zSlice = 0) = 0;
	virtual void AttachRenderBuffer(GLenum attachment, GLuint rboId) = 0;
	virtual void Detach(GLenum attachment) = 0;
	virtual bool IsValid() const = 0;
	virtual bool IsComplete(const char* name) = 0;
	virtual int2 GetSize() const = 0;
	virtual void SetSize(const int2& size) = 0;
	virtual void SetDrawBuffers(unsigned int count, const GLenum* attachments) = 0;
	virtual uint32_t GetId() const = 0;

	virtual void AttachColor(unsigned int idx, GLuint texId, GLuint texTarget)
	{
		AttachTexture(GL_COLOR_ATTACHMENT0_EXT + idx, texId, texTarget);
	}

	virtual void AttachDepth(GLuint texId, GLuint texTarget)
	{
		AttachTexture(GL_DEPTH_ATTACHMENT_EXT, texId, texTarget);
	}
};
