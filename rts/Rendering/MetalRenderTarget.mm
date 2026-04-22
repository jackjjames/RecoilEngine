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

	void ClearColor(const float4& newColor) override
	{
		clearColor = newColor;
	}

	void ClearDepth(float newDepth) override
	{
		clearDepth = newDepth;
	}

	void SetBlendState(const RenderTargetBlendState& newState) override
	{
		blendState = newState;
	}

	void SetDepthState(const RenderTargetDepthState& newState) override
	{
		depthState = newState;
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
	float4 clearColor = float4(1.0f, 0.0f, 1.0f, 1.0f);
	float clearDepth = 1.0f;
	RenderTargetBlendState blendState;
	RenderTargetDepthState depthState;
};

} // namespace

std::unique_ptr<IRenderTarget> CreateMetalRenderTarget()
{
	return std::make_unique<MetalRenderTarget>();
}

std::unique_ptr<IRenderTarget> CreateMetalDefaultRenderTarget()
{
	auto renderTarget = std::make_unique<MetalRenderTarget>();
	renderTarget->AttachNativeColor(0);
	return renderTarget;
}
