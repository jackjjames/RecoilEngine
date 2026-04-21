#include "Rendering/Platform/IPresenter.h"

#include "Rendering/GlobalRendering.h"

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
	}

	void PresentFrame(CGlobalRendering& rendering, bool allowSwapBuffers, bool clearErrors) const override
	{
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
