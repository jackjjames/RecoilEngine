/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <memory>

#include "System/type2.h"

struct SDL_Window;
using NativeRenderContextHandle = void*;

class CGlobalRendering;
class IRenderTarget;

class IRenderContext
{
public:
	virtual ~IRenderContext() = default;

	virtual SDL_Window* CreateWindow(const CGlobalRendering& rendering, const char* title) const = 0;
	virtual NativeRenderContextHandle CreateContext(const CGlobalRendering& rendering, SDL_Window* window, const int2& minCtx) const = 0;
	virtual void InitializeNativeContext(CGlobalRendering& rendering) const = 0;
	virtual void DestroyWindowAndContext(CGlobalRendering& rendering) const = 0;
	virtual void KillSDL() const = 0;
	virtual void MakeCurrent(SDL_Window* window, NativeRenderContextHandle context, bool clear) const = 0;
	virtual void SwapWindow(SDL_Window* window) const = 0;
	virtual IRenderTarget& GetDefaultRenderTarget(const CGlobalRendering& rendering) const = 0;
	virtual SDL_Window* GetNativeWindow(const CGlobalRendering& rendering) const = 0;
	virtual NativeRenderContextHandle GetNativeContext(const CGlobalRendering& rendering) const = 0;
};
