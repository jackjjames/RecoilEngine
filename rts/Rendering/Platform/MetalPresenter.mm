#include "Rendering/Platform/IPresenter.h"

#include "Rendering/GlobalRendering.h"
#include "Rendering/Platform/MetalFrameControl.h"

#include <memory>

namespace {

class MetalPresenter final : public IPresenter
{
public:
	void RefreshGLState(CGlobalRendering& rendering) const override
	{
		rendering.UpdateWindow();
	}

	void BeginFrame(CGlobalRendering& rendering) const override
	{
		rendering.UpdateWindow();
		rendering.UpdateTimer();
		MetalFrame::Begin(rendering.sdlWindow);
	}

	void PresentFrame(CGlobalRendering& rendering, bool allowSwapBuffers, bool clearErrors) const override
	{
		// SwapBuffers funnels into IRenderContext::SwapWindow which calls
		// MetalFrame::End, committing the in-flight command buffer and
		// presenting the drawable acquired in BeginFrame.
		rendering.SwapBuffers(allowSwapBuffers, clearErrors);
	}

	void OnResize(CGlobalRendering& rendering, int, int) const override
	{
		rendering.UpdateWindow();
	}
};

} // namespace

std::unique_ptr<IPresenter> CreateMetalPresenter()
{
	return std::make_unique<MetalPresenter>();
}
