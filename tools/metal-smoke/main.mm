// metal-smoke: standalone harness that exercises the engine's Metal backend
// bits in isolation, without booting CGlobalRendering / configHandler / the
// full Spring app. Stage 6 commits grow the harness slice-by-slice until a
// single triangle draws into an offscreen texture and a center-pixel readback
// confirms pixels landed.
//
// This slice (S6-C4) builds the same PipelineDesc as the engine's
// TrianglePass, allocates the uniform buffer through CreateMetalBuffer, and
// issues the draw through MetalFrame::BeginOffscreen / EndOffscreen against
// a 256x256 shared-storage MTLTexture. The draw is sanity-checked (no Metal
// errors, pipeline valid, frame command buffer completed); pixel readback is
// a follow-up slice.

#include "Rendering/IBuffer.h"
#include "Rendering/MetalRenderGlobals.h"
#include "Rendering/Platform/MetalFrameControl.h"
#include "Rendering/Shaders/IShaderPipeline.h"

#include <SDL.h>
#include <SDL_metal.h>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdio>
#include <cstdlib>
#include <memory>

// Declared in MetalBuffer.mm / MetalShaderPipeline.mm (compiled directly into
// this tool target). We reach for the factory functions instead of
// IRenderBackend so the harness does not drag in CGlobalRendering.
std::unique_ptr<IBuffer> CreateMetalBuffer(size_t size, const void* data);
std::unique_ptr<IShaderPipeline> CreateMetalShaderPipeline(const PipelineDesc& desc);

namespace {

constexpr int kWindowWidth  = 256;
constexpr int kWindowHeight = 256;
constexpr NSUInteger kRenderTargetSize = 256;

struct alignas(16) TrianglePassUniforms
{
	float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

PipelineDesc BuildTrianglePipelineDesc()
{
	// Kept byte-for-byte in sync with rts/Rendering/Debug/TrianglePass.cpp so
	// a smoke-tool pass is representative of what the real engine path does.
	return {
		.name = "TrianglePass",
		.vertexSource = R"(
#version 450 core
const vec2 positions[3] = vec2[3](
	vec2(-0.75, -0.75),
	vec2( 0.75, -0.75),
	vec2( 0.00,  0.75)
);
void main() {
	gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
)",
		.fragmentSource = R"(
#version 450 core
layout(std140, binding = 0) uniform TrianglePassData {
	vec4 color;
};
layout(location = 0) out vec4 outColor;
void main() {
	outColor = color;
}
)",
	};
}

struct SmokeContext {
	SDL_Window*         window       = nullptr;
	SDL_MetalView       metalView    = nullptr;
	id<MTLDevice>       device       = nil;
	id<MTLCommandQueue> commandQueue = nil;
	CAMetalLayer*       layer        = nil;
	id<MTLTexture>      renderTarget = nil;
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

	if (ctx.renderTarget != nil) {
		[ctx.renderTarget release];
		ctx.renderTarget = nil;
	}
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

id<MTLTexture> MakeOffscreenTarget(id<MTLDevice> device)
{
	MTLTextureDescriptor* desc =
		[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		                                                   width:kRenderTargetSize
		                                                  height:kRenderTargetSize
		                                               mipmapped:NO];
	desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	// Shared storage is an Apple-Silicon special: the same backing pages are
	// accessible to CPU and GPU without blit-sync, which keeps the readback
	// slice on C5 trivial. If we ever extend the smoke to intel macs we will
	// need MTLStorageModeManaged + a blit synchronize step.
	desc.storageMode = MTLStorageModeShared;
	return [device newTextureWithDescriptor:desc];
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
	if (ctx.window == nullptr) { Teardown(ctx); Fail("SDL_CreateWindow", SDL_GetError()); }

	ctx.metalView = SDL_Metal_CreateView(ctx.window);
	if (ctx.metalView == nullptr) { Teardown(ctx); Fail("SDL_Metal_CreateView", SDL_GetError()); }

	ctx.layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(ctx.metalView);
	if (ctx.layer == nil) { Teardown(ctx); Fail("SDL_Metal_GetLayer", "nil layer"); }

	ctx.device = MTLCreateSystemDefaultDevice();
	if (ctx.device == nil) { Teardown(ctx); Fail("MTLCreateSystemDefaultDevice", "no default device"); }

	ctx.commandQueue = [ctx.device newCommandQueue];
	if (ctx.commandQueue == nil) { Teardown(ctx); Fail("newCommandQueue", "nil queue"); }

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

	ctx.renderTarget = MakeOffscreenTarget(ctx.device);
	if (ctx.renderTarget == nil) { Teardown(ctx); Fail("newTextureWithDescriptor", "nil"); }

	auto pipeline = CreateMetalShaderPipeline(BuildTrianglePipelineDesc());
	if (pipeline == nullptr || !pipeline->IsValid()) {
		std::fprintf(stderr, "metal-smoke: pipeline log: %s\n",
			pipeline ? pipeline->GetLog().c_str() : "<null>");
		Teardown(ctx);
		Fail("CreateMetalShaderPipeline", "pipeline not valid");
	}

	TrianglePassUniforms uniforms;
	auto buffer = CreateMetalBuffer(sizeof(uniforms), &uniforms);
	if (buffer == nullptr || !buffer->IsValid()) {
		Teardown(ctx);
		Fail("CreateMetalBuffer", "buffer not valid");
	}

	MetalFrame::BeginOffscreen((__bridge void*)ctx.renderTarget,
	                            (__bridge void*)ctx.commandQueue);

	pipeline->BindUniformBuffer(0, *buffer, 0, sizeof(uniforms));
	pipeline->Enable();
	pipeline->Draw(PrimitiveTopology::Triangles, 0, 3);
	pipeline->Disable();

	MetalFrame::EndOffscreen();

	std::printf("metal-smoke: pipeline log = %s\n", pipeline->GetLog().c_str());
	std::printf("metal-smoke: offscreen draw committed\n");

	Teardown(ctx);
	std::puts("metal-smoke: ok");
	return 0;
}
