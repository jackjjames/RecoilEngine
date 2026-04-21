#include "Rendering/IRenderTarget.h"

#include <memory>

namespace {

class MetalRenderTarget final : public IRenderTarget
{
public:
	void Bind() override
	{
	}

	void Unbind() override
	{
	}

	void AttachNativeColor(unsigned int idx) override
	{
		nativeColorAttachment = idx;
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

	void SetDrawBuffers(unsigned int, const GLenum*) override
	{
	}

	uint32_t GetId() const override
	{
		return nativeColorAttachment;
	}

private:
	int2 size = {0, 0};
	uint32_t nativeColorAttachment = 0;
};

} // namespace

std::unique_ptr<IRenderTarget> CreateMetalRenderTarget()
{
	return std::make_unique<MetalRenderTarget>();
}
