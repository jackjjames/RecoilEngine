// metal-smoke: standalone harness that exercises the engine's Metal backend
// bits in isolation, without booting CGlobalRendering / configHandler / the
// full Spring app. Stage 6 commits it slice-by-slice until a single triangle
// draws into an offscreen texture and a center-pixel readback confirms pixels
// landed.
//
// This slice (S6-C2) opens an SDL metal window, creates an MTLDevice and an
// MTLCommandQueue, publishes them through MetalGlobals so the shared Metal
// backend primitives (MetalBuffer, MetalShaderPipeline) can pick them up in
// later slices, and then tears everything down cleanly.

#include "Rendering/MetalRenderGlobals.h"

#include <SDL.h>
#include <SDL_metal.h>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdio>
#include <cstdlib>

namespace {

constexpr int kWindowWidth  = 256;
constexpr int kWindowHeight = 256;

struct SmokeContext {
	SDL_Window*         window       = nullptr;
	SDL_MetalView       metalView    = nullptr;
	id<MTLDevice>       device       = nil;
	id<MTLCommandQueue> commandQueue = nil;
	CAMetalLayer*       layer        = nil;
};

[[noreturn]] void Fail(const char* where, const char* detail)
{
	std::fprintf(stderr, "metal-smoke: %s failed: %s\n", where, detail ? detail : "");
	std::exit(1);
}

void Teardown(SmokeContext& ctx)
{
	MetalGlobals::SetCurrentEncoder(nullptr);
	MetalGlobals::SetCurrentCommandBuffer(nullptr);
	MetalGlobals::SetCurrentDrawable(nullptr);
	MetalGlobals::SetCurrentPipelineState(nullptr);
	MetalGlobals::ClearBindings();
	MetalGlobals::SetCommandQueue(nullptr);
	MetalGlobals::SetDevice(nullptr);

	if (ctx.commandQueue != nil) {
		[ctx.commandQueue release];
		ctx.commandQueue = nil;
	}
	if (ctx.device != nil) {
		[ctx.device release];
		ctx.device = nil;
	}
	if (ctx.metalView != nullptr) {
		SDL_Metal_DestroyView(ctx.metalView);
		ctx.metalView = nullptr;
	}
	if (ctx.window != nullptr) {
		SDL_DestroyWindow(ctx.window);
		ctx.window = nullptr;
	}

	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	SDL_Quit();
}

} // namespace

int main(int /*argc*/, char** /*argv*/)
{
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
		Fail("SDL_Init(VIDEO)", SDL_GetError());

	SmokeContext ctx{};

	const uint32_t flags = SDL_WINDOW_METAL | SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI;
	ctx.window = SDL_CreateWindow(
		"metal-smoke",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		kWindowWidth, kWindowHeight,
		flags);
	if (ctx.window == nullptr) {
		Teardown(ctx);
		Fail("SDL_CreateWindow", SDL_GetError());
	}

	ctx.metalView = SDL_Metal_CreateView(ctx.window);
	if (ctx.metalView == nullptr) {
		Teardown(ctx);
		Fail("SDL_Metal_CreateView", SDL_GetError());
	}

	ctx.layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(ctx.metalView);
	if (ctx.layer == nil) {
		Teardown(ctx);
		Fail("SDL_Metal_GetLayer", "nil layer");
	}

	ctx.device = MTLCreateSystemDefaultDevice();
	if (ctx.device == nil) {
		Teardown(ctx);
		Fail("MTLCreateSystemDefaultDevice", "no default device");
	}

	ctx.commandQueue = [ctx.device newCommandQueue];
	if (ctx.commandQueue == nil) {
		Teardown(ctx);
		Fail("newCommandQueue", "nil queue");
	}

	ctx.layer.device = ctx.device;
	ctx.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	ctx.layer.framebufferOnly = NO;

	int drawableW = 0;
	int drawableH = 0;
	SDL_Metal_GetDrawableSize(ctx.window, &drawableW, &drawableH);
	ctx.layer.drawableSize = CGSizeMake(drawableW, drawableH);

	MetalGlobals::SetDevice((__bridge void*)ctx.device);
	MetalGlobals::SetCommandQueue((__bridge void*)ctx.commandQueue);

	std::printf("metal-smoke: device       = %s\n", [[ctx.device name] UTF8String]);
	std::printf("metal-smoke: drawable     = %dx%d\n", drawableW, drawableH);
	std::printf("metal-smoke: MetalGlobals device=%p queue=%p\n",
		MetalGlobals::GetDevice(), MetalGlobals::GetCommandQueue());

	if (MetalGlobals::GetDevice() == nullptr || MetalGlobals::GetCommandQueue() == nullptr) {
		Teardown(ctx);
		Fail("MetalGlobals", "device/queue not published");
	}

	Teardown(ctx);
	std::puts("metal-smoke: ok");
	return 0;
}
