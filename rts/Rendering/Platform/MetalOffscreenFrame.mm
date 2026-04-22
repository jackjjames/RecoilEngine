/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Rendering/MetalRenderGlobals.h"
#include "Rendering/Platform/MetalFrameControl.h"

#import <Metal/Metal.h>

// Offscreen frame helpers for MetalFrame. Kept in their own translation unit
// (rather than folded into MetalRenderContext.mm) so standalone tools like
// tools/metal-smoke can pull in just these helpers without dragging in the
// window-bound MetalRenderContext class, which depends on configHandler /
// CGlobalRendering.

namespace MetalFrame {

void BeginOffscreen(void* targetTexture, void* commandQueue)
{
	auto target = (__bridge id<MTLTexture>)targetTexture;
	auto queue  = (__bridge id<MTLCommandQueue>)commandQueue;
	if (target == nil || queue == nil)
		return;

	MTLRenderPassDescriptor* renderPass = [MTLRenderPassDescriptor renderPassDescriptor];
	renderPass.colorAttachments[0].texture = target;
	renderPass.colorAttachments[0].loadAction = MTLLoadActionClear;
	renderPass.colorAttachments[0].storeAction = MTLStoreActionStore;
	renderPass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

	id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
	id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPass];

	// Match Begin()'s manual retain policy: translation units participating in
	// the engine build compile without ARC, so we keep the objects alive until
	// the matching EndOffscreen call releases them.
	[commandBuffer retain];
	[encoder retain];

	MetalGlobals::SetCurrentDrawable(nullptr);
	MetalGlobals::SetCurrentCommandBuffer((__bridge void*)commandBuffer);
	MetalGlobals::SetCurrentEncoder((__bridge void*)encoder);
}

void EndOffscreen()
{
	auto encoder       = (__bridge id<MTLRenderCommandEncoder>)MetalGlobals::GetCurrentEncoder();
	auto commandBuffer = (__bridge id<MTLCommandBuffer>)MetalGlobals::GetCurrentCommandBuffer();

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
		[commandBuffer commit];
		[commandBuffer waitUntilCompleted];
		[commandBuffer release];
	}
}

} // namespace MetalFrame
