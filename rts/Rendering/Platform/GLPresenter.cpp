/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Rendering/Platform/GLPresenter.h"

#include "Rendering/Platform/IPresenter.h"
#include "Rendering/GlobalRendering.h"

#include <memory>

namespace {

class GLPresenter final : public IPresenter
{
public:
	void RefreshGLState(CGlobalRendering& rendering) const override
	{
		rendering.UpdateGLConfigs();
		rendering.UpdateGLGeometry();
		rendering.InitGLState();
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
		RefreshGLState(rendering);
	}
};

} // namespace

std::unique_ptr<IPresenter> CreateGLPresenter()
{
	return std::make_unique<GLPresenter>();
}
