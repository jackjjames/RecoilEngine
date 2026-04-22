/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

// Helpers that the Metal presenter + render context use to drive the per-frame
// encoder lifecycle. The MTLRenderCommandEncoder is owned by the context for
// the duration of the frame and published through MetalRenderGlobals so
// pipelines and buffers can bind onto it.

struct SDL_Window;

namespace MetalFrame {

// Acquire a drawable, start a command buffer + render encoder with a clear
// load action, and publish the encoder/command buffer/drawable onto the
// MetalGlobals frame slots. No-op if the window/context is not ready.
void Begin(SDL_Window* window);

// End encoding, commit the command buffer with presentDrawable, and clear the
// frame globals. Safe to call without a prior Begin.
void End(SDL_Window* window);

// Offscreen variant: render into a caller-owned MTLTexture using the supplied
// command queue. Publishes the encoder / command buffer onto MetalGlobals the
// same way Begin() does, but no drawable is acquired and EndOffscreen does
// not call presentDrawable. Used by the metal-smoke harness and by future
// offscreen passes (screenshots, RTT probes) that need Metal without a
// swapchain. targetTexture is id<MTLTexture>, commandQueue is
// id<MTLCommandQueue>; both are passed as void* to keep this header
// ObjC-free. Uses MTLLoadActionClear with a black clear colour.
void BeginOffscreen(void* targetTexture, void* commandQueue);

// Counterpart to BeginOffscreen: end encoding, commit the command buffer, and
// wait for completion so callers can read pixels back immediately after the
// call returns. Clears the frame globals like End() does.
void EndOffscreen();

} // namespace MetalFrame
