/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

class CGlobalRendering;

class IPresenter
{
public:
	virtual ~IPresenter() = default;

	virtual void RefreshGLState(CGlobalRendering& rendering) const = 0;
	virtual void BeginFrame(CGlobalRendering& rendering) const = 0;
	virtual void PresentFrame(CGlobalRendering& rendering, bool allowSwapBuffers, bool clearErrors) const = 0;
	virtual void OnResize(CGlobalRendering& rendering, int width, int height) const = 0;
};
