#include "Rendering/Platform/IRenderContext.h"

#include "Rendering/GlobalRendering.h"
#include "Rendering/IRenderTarget.h"
#include "Rendering/MetalRenderGlobals.h"
#include "Rendering/Platform/MetalFrameControl.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"
#include "System/Platform/WindowManagerHelper.h"
#include "System/Platform/errorhandler.h"

#include <memory>

#include <SDL.h>
#include <SDL_metal.h>
#include <fmt/format.h>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

std::unique_ptr<IRenderTarget> CreateMetalDefaultRenderTarget();

namespace {

struct MetalContextState {
	SDL_MetalView metalView = nullptr;
	void* layer = nullptr;
	void* device = nullptr;
	void* commandQueue = nullptr;
};

static constexpr const char* metalStateKey = "spring-metal-state";

static inline MetalContextState* GetMetalState(SDL_Window* window)
{
	return static_cast<MetalContextState*>(SDL_GetWindowData(window, metalStateKey));
}

static inline void UpdateDrawableSize(SDL_Window* window, CAMetalLayer* layer)
{
	int width = 0;
	int height = 0;
	SDL_Metal_GetDrawableSize(window, &width, &height);
	layer.drawableSize = CGSizeMake(width, height);
}

class MetalRenderContext final : public IRenderContext
{
public:
	SDL_Window* CreateWindow(const CGlobalRendering& rendering, const char* title) const override
	{
		const int2 newRes = rendering.GetCfgWinRes();
		const bool borderless = configHandler->GetBool("WindowBorderless");
		const bool fullScreen = configHandler->GetBool("Fullscreen");
		const int winPosX = configHandler->GetInt("WindowPosX");
		const int winPosY = configHandler->GetInt("WindowPosY");

		uint32_t sdlFlags = SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
		sdlFlags |= (borderless ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN) * fullScreen;
		sdlFlags |= (SDL_WINDOW_BORDERLESS * borderless);

		SDL_Window* window = SDL_CreateWindow(title, winPosX, winPosY, newRes.x, newRes.y, sdlFlags);

		if (window == nullptr) {
			const auto err = fmt::format("[GR::{}] could not create SDL metal window: {}", __func__, SDL_GetError());
			handleerror(nullptr, err.c_str(), "ERROR", MBF_OK | MBF_EXCL);
			return nullptr;
		}

		rendering.UpdateWindowBorders(window);
		return window;
	}

	NativeRenderContextHandle CreateContext(const CGlobalRendering&, SDL_Window* window, const int2&) const override
	{
		auto* state = new MetalContextState();

		state->metalView = SDL_Metal_CreateView(window);
		if (state->metalView == nullptr) {
			delete state;
			return nullptr;
		}

		CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(state->metalView);
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();

		if (layer == nil || device == nil) {
			SDL_Metal_DestroyView(state->metalView);
			delete state;
			return nullptr;
		}

		id<MTLCommandQueue> commandQueue = [device newCommandQueue];
		if (commandQueue == nil) {
			SDL_Metal_DestroyView(state->metalView);
			delete state;
			return nullptr;
		}

		layer.device = device;
		layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
		layer.framebufferOnly = NO;
		UpdateDrawableSize(window, layer);

		state->layer = (__bridge void*)layer;
		state->device = (__bridge_retained void*)device;
		state->commandQueue = (__bridge void*)commandQueue;

		MetalGlobals::SetDevice((__bridge void*)device);
		MetalGlobals::SetCommandQueue((__bridge void*)commandQueue);
		MetalGlobals::SetLayer((__bridge void*)layer);

		SDL_SetWindowData(window, metalStateKey, state);

		int drawableW = 0;
		int drawableH = 0;
		SDL_Metal_GetDrawableSize(window, &drawableW, &drawableH);
		LOG("[GR::MetalRenderContext] device=\"%s\" drawable=%dx%d pixelFormat=BGRA8Unorm",
			[[device name] UTF8String], drawableW, drawableH);

		return state;
	}

	void InitializeNativeContext(CGlobalRendering& rendering) const override
	{
		if (auto* state = static_cast<MetalContextState*>(rendering.glContext)) {
			auto* layer = (__bridge CAMetalLayer*)state->layer;
			UpdateDrawableSize(rendering.sdlWindow, layer);
		}
	}

	void DestroyWindowAndContext(CGlobalRendering& rendering) const override
	{
		if (!rendering.sdlWindow)
			return;

		WindowManagerHelper::SetIconSurface(rendering.sdlWindow, nullptr);
		rendering.SetWindowInputGrabbing(false);

		if (auto* state = static_cast<MetalContextState*>(rendering.glContext)) {
			SDL_SetWindowData(rendering.sdlWindow, metalStateKey, nullptr);

			MetalGlobals::SetCurrentEncoder(nullptr);
			MetalGlobals::SetLayer(nullptr);
			MetalGlobals::SetCommandQueue(nullptr);
			MetalGlobals::SetDevice(nullptr);
			MetalGlobals::ClearBindings();

			if (state->metalView != nullptr)
				SDL_Metal_DestroyView(state->metalView);
			if (state->commandQueue != nullptr)
				[(__bridge id)state->commandQueue release];
			if (state->device != nullptr)
				[(__bridge id)state->device release];

			delete state;
		}

		SDL_DestroyWindow(rendering.sdlWindow);
		rendering.sdlWindow = nullptr;
		rendering.glContext = nullptr;
	}

	void KillSDL() const override
	{
#if !defined(HEADLESS)
		SDL_QuitSubSystem(SDL_INIT_VIDEO);
#endif

		SDL_EnableScreenSaver();
		SDL_Quit();
	}

	void MakeCurrent(SDL_Window*, NativeRenderContextHandle, bool) const override
	{
	}

	void SwapWindow(SDL_Window* window) const override
	{
		MetalFrame::End(window);
	}

	IRenderTarget& GetDefaultRenderTarget(const CGlobalRendering& rendering) const override
	{
		defaultRenderTarget->SetSize({rendering.viewSizeX, rendering.viewSizeY});
		return *defaultRenderTarget;
	}

	SDL_Window* GetNativeWindow(const CGlobalRendering& rendering) const override
	{
		return rendering.sdlWindow;
	}

	NativeRenderContextHandle GetNativeContext(const CGlobalRendering& rendering) const override
	{
		return rendering.glContext;
	}

private:
	mutable std::unique_ptr<IRenderTarget> defaultRenderTarget = CreateMetalDefaultRenderTarget();
};

} // namespace

std::unique_ptr<IRenderContext> CreateMetalRenderContext()
{
	return std::make_unique<MetalRenderContext>();
}

namespace MetalFrame {

void Begin(SDL_Window* window)
{
	if (window == nullptr)
		return;
	auto* state = GetMetalState(window);
	if (state == nullptr)
		return;

	auto* layer = (__bridge CAMetalLayer*)state->layer;
	auto commandQueue = (__bridge id<MTLCommandQueue>)state->commandQueue;
	if (layer == nil || commandQueue == nil)
		return;

	UpdateDrawableSize(window, layer);

	id<CAMetalDrawable> drawable = [layer nextDrawable];
	if (drawable == nil)
		return;

	// Translation units in this engine are compiled without ARC, so we keep
	// the drawable/command buffer/encoder alive manually for the duration of
	// the frame: retain on Begin, release on End. Autorelease-pool churn
	// would otherwise drop them between the encoder record and the commit.
	MTLRenderPassDescriptor* renderPass = [MTLRenderPassDescriptor renderPassDescriptor];
	renderPass.colorAttachments[0].texture = drawable.texture;
	renderPass.colorAttachments[0].loadAction = MTLLoadActionClear;
	renderPass.colorAttachments[0].storeAction = MTLStoreActionStore;
	renderPass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

	id<MTLCommandBuffer> commandBuffer = [commandQueue commandBuffer];
	id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPass];

	[drawable retain];
	[commandBuffer retain];
	[encoder retain];

	MetalGlobals::SetCurrentDrawable((__bridge void*)drawable);
	MetalGlobals::SetCurrentCommandBuffer((__bridge void*)commandBuffer);
	MetalGlobals::SetCurrentEncoder((__bridge void*)encoder);
}

void End(SDL_Window* window)
{
	(void)window;

	auto encoder       = (__bridge id<MTLRenderCommandEncoder>)MetalGlobals::GetCurrentEncoder();
	auto commandBuffer = (__bridge id<MTLCommandBuffer>)MetalGlobals::GetCurrentCommandBuffer();
	auto drawable      = (__bridge id<CAMetalDrawable>)MetalGlobals::GetCurrentDrawable();

	MetalGlobals::SetCurrentEncoder(nullptr);
	MetalGlobals::SetCurrentCommandBuffer(nullptr);
	MetalGlobals::SetCurrentDrawable(nullptr);
	MetalGlobals::SetCurrentPipelineState(nullptr);
	MetalGlobals::ClearBindings();

	if (encoder != nil) {
		[encoder endEncoding];
		[encoder release];
	}

	if (commandBuffer != nil) {
		if (drawable != nil)
			[commandBuffer presentDrawable:drawable];
		[commandBuffer commit];
		[commandBuffer release];
	}

	if (drawable != nil)
		[drawable release];
}

} // namespace MetalFrame
