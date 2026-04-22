/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "GLRenderTarget.h"

#include "Rendering/IRenderTarget.h"
#include "Rendering/GL/FBO.h"

#include <memory>

namespace {

class GLRenderTargetBase : public IRenderTarget
{
public:
	void ClearColor(const float4& color) override
	{
		glClearColor(color.x, color.y, color.z, color.w);
		glClear(GL_COLOR_BUFFER_BIT);
	}

	void ClearDepth(float depth) override
	{
		glClearDepth(depth);
		glClear(GL_DEPTH_BUFFER_BIT);
	}

	void SetBlendState(const RenderTargetBlendState& state) override
	{
		if (state.enabled) {
			glEnable(GL_BLEND);
			glBlendFuncSeparate(state.srcColor, state.dstColor, state.srcAlpha, state.dstAlpha);
		} else {
			glDisable(GL_BLEND);
		}
	}

	void SetDepthState(const RenderTargetDepthState& state) override
	{
		glDepthMask(state.writeEnabled);

		if (state.testEnabled) {
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(state.func);
		} else {
			glDisable(GL_DEPTH_TEST);
		}
	}
};

class GLRenderTarget final : public GLRenderTargetBase
{
public:
	void Bind() override
	{
		fbo.Bind();
	}

	void Unbind() override
	{
		fbo.Unbind();
	}

	void AttachNativeColor(unsigned int) override
	{
		// GL render targets only attach explicit textures or renderbuffers.
	}

	void AttachTexture(GLenum attachment, GLuint texId, GLuint texTarget, int mipLevel, int zSlice) override
	{
		fbo.AttachTexture(texId, texTarget, attachment, mipLevel, zSlice);
	}

	void AttachRenderBuffer(GLenum attachment, GLuint rboId) override
	{
		fbo.AttachRenderBuffer(rboId, attachment);
	}

	void Detach(GLenum attachment) override
	{
		fbo.Detach(attachment);
	}

	bool IsValid() const override
	{
		return fbo.IsValid();
	}

	bool IsComplete(const char* name) override
	{
		return fbo.CheckStatus(name);
	}

	int2 GetSize() const override
	{
		return size;
	}

	void SetSize(const int2& newSize) override
	{
		size = newSize;
	}

	void SetDrawBuffers(unsigned int count, const GLenum* attachments) override
	{
		glDrawBuffers(count, attachments);
	}

	uint32_t GetId() const override
	{
		return fbo.GetId();
	}

private:
	FBO fbo;
	int2 size = {0, 0};
};

class GLDefaultRenderTarget final : public GLRenderTargetBase
{
public:
	void Bind() override
	{
		FBO::Unbind();
	}

	void Unbind() override
	{
		FBO::Unbind();
	}

	void AttachNativeColor(unsigned int) override
	{
	}

	void AttachTexture(GLenum, GLuint, GLuint, int, int) override
	{
	}

	void AttachRenderBuffer(GLenum, GLuint) override
	{
	}

	void Detach(GLenum) override
	{
	}

	bool IsValid() const override
	{
		return true;
	}

	bool IsComplete(const char*) override
	{
		return true;
	}

	int2 GetSize() const override
	{
		return size;
	}

	void SetSize(const int2& newSize) override
	{
		size = newSize;
	}

	void SetDrawBuffers(unsigned int count, const GLenum* attachments) override
	{
		if (count == 0) {
			glDrawBuffer(GL_NONE);
			return;
		}

		glDrawBuffer(attachments[0]);
	}

	uint32_t GetId() const override
	{
		return 0;
	}

private:
	int2 size = {0, 0};
};

} // namespace

std::unique_ptr<IRenderTarget> CreateGLRenderTarget()
{
	return std::make_unique<GLRenderTarget>();
}

std::unique_ptr<IRenderTarget> CreateGLDefaultRenderTarget()
{
	return std::make_unique<GLDefaultRenderTarget>();
}
