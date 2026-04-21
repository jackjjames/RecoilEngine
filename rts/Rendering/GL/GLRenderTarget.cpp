/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "GLRenderTarget.h"

#include "Rendering/IRenderTarget.h"
#include "Rendering/GL/FBO.h"

#include <memory>

namespace {

class GLRenderTarget final : public IRenderTarget
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

	void AttachColor(unsigned int idx, GLuint texId, GLuint texTarget) override
	{
		fbo.AttachTexture(texId, texTarget, GL_COLOR_ATTACHMENT0_EXT + idx);
	}

	void AttachDepth(GLuint texId, GLuint texTarget) override
	{
		fbo.AttachTexture(texId, texTarget, GL_DEPTH_ATTACHMENT_EXT);
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

} // namespace

std::unique_ptr<IRenderTarget> CreateGLRenderTarget()
{
	return std::make_unique<GLRenderTarget>();
}
