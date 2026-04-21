/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <memory>

#include "Rendering/Platform/IRenderContext.h"
#include "Rendering/Platform/IPresenter.h"

class IRenderTarget;

class IRenderBackend
{
public:
	virtual ~IRenderBackend() = default;

	virtual IRenderContext& GetRenderContext() = 0;
	virtual const IRenderContext& GetRenderContext() const = 0;
	virtual IPresenter& GetPresenter() = 0;
	virtual const IPresenter& GetPresenter() const = 0;
	virtual std::unique_ptr<IRenderTarget> CreateRenderTarget() const = 0;
};
