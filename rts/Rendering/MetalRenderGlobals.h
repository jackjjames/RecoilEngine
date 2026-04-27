/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

// Tiny facade the Metal backend uses to hand the MTLDevice / active encoder
// to the buffer + pipeline stubs without threading them through every call
// site. The GL build never compiles this file; it only exists under
// APPLE + SPRING_RENDER_BACKEND=metal.
//
// The primitives are intentionally opaque `void*` on the C++ side so callers
// that do not want to drag <Metal/Metal.h> into their translation unit can
// still participate. The .mm implementation casts back to the real id<...>
// types.

#include <cstddef>
#include <cstdint>

namespace MetalGlobals {

void SetDevice(void* device);
void* GetDevice();

void SetCommandQueue(void* queue);
void* GetCommandQueue();

// The swapchain CAMetalLayer published by the engine's MetalRenderContext
// at window-create time. Null outside of window lifetime. Useful for engine
// code that needs to query drawable size / pixel format without reaching
// back to SDL_GetWindowData.
void SetLayer(void* layer);
void* GetLayer();

// The active MTLRenderCommandEncoder for the in-flight frame, if any.
// Nullable outside of BeginFrame/EndFrame.
void SetCurrentEncoder(void* encoder);
void* GetCurrentEncoder();

void SetCurrentScissorRect(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

// Pending uniform-buffer bindings accumulate on the context so draw calls can
// replay them onto the encoder once a pipeline is in flight. We keep this in a
// fixed-size array because Metal's vertex+fragment buffer slots both max out at
// 31 (kMTLMaxBuffers in practice).
static constexpr uint32_t kMaxBindSlots = 8;

struct BufferBinding {
	void*    mtlBuffer = nullptr; // id<MTLBuffer>
	size_t   offset    = 0;
	size_t   size      = 0;
};

void SetUniformBinding(uint32_t slot, const BufferBinding& binding);
const BufferBinding& GetUniformBinding(uint32_t slot);

// Vertex buffer bindings. `slot` matches VertexBindingLayout::slot from the
// PipelineDesc; the pipeline maps this onto the actual MTL buffer index at
// draw time (offsetting by metalVertexBufferBaseSlot). Bindings live on the
// global facade so callers that don't own the encoder can still participate.
void SetVertexBufferBinding(uint32_t slot, const BufferBinding& binding);
const BufferBinding& GetVertexBufferBinding(uint32_t slot);

// Sampled-texture bindings for the fragment stage. `slot` is the
// `[[texture(N)]]` index on the MSL side. `mtlTexture` is id<MTLTexture>.
struct TextureBinding {
	void* mtlTexture = nullptr; // id<MTLTexture>
	void* mtlSampler = nullptr; // id<MTLSamplerState>
};

void SetTextureBinding(uint32_t slot, const TextureBinding& binding);
const TextureBinding& GetTextureBinding(uint32_t slot);

void ClearBindings();

// The pipeline currently targeted by Enable() -- draw replays it onto the
// encoder at submission time. The pointer is an id<MTLRenderPipelineState>.
void SetCurrentPipelineState(void* pipelineState);
void* GetCurrentPipelineState();

// The in-flight CAMetalDrawable and MTLCommandBuffer, published by
// BeginFrame and consumed by EndFrame. Null outside of a frame.
void SetCurrentDrawable(void* drawable);
void* GetCurrentDrawable();

void SetCurrentCommandBuffer(void* commandBuffer);
void* GetCurrentCommandBuffer();

} // namespace MetalGlobals
