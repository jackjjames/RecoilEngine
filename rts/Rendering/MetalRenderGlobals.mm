/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Rendering/MetalRenderGlobals.h"

#import <Metal/Metal.h>

#include <array>

namespace MetalGlobals {

namespace {

void* g_device = nullptr;
void* g_commandQueue = nullptr;
void* g_layer = nullptr;
void* g_currentEncoder = nullptr;
void* g_currentPipelineState = nullptr;
void* g_currentDrawable = nullptr;
void* g_currentCommandBuffer = nullptr;

std::array<BufferBinding, kMaxBindSlots> g_uniformBindings{};
std::array<BufferBinding, kMaxBindSlots> g_vertexBufferBindings{};
std::array<TextureBinding, kMaxBindSlots> g_textureBindings{};

} // namespace

void SetDevice(void* device) { g_device = device; }
void* GetDevice() { return g_device; }

void SetCommandQueue(void* queue) { g_commandQueue = queue; }
void* GetCommandQueue() { return g_commandQueue; }

void SetLayer(void* layer) { g_layer = layer; }
void* GetLayer() { return g_layer; }

void SetCurrentEncoder(void* encoder) { g_currentEncoder = encoder; }
void* GetCurrentEncoder() { return g_currentEncoder; }

void SetCurrentScissorRect(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	auto encoder = (__bridge id<MTLRenderCommandEncoder>)g_currentEncoder;
	if (encoder == nil)
		return;

	MTLScissorRect rect;
	rect.x = x;
	rect.y = y;
	rect.width = width;
	rect.height = height;
	[encoder setScissorRect:rect];
}

void SetUniformBinding(uint32_t slot, const BufferBinding& binding)
{
	if (slot >= kMaxBindSlots)
		return;
	g_uniformBindings[slot] = binding;
}

const BufferBinding& GetUniformBinding(uint32_t slot)
{
	static const BufferBinding empty{};
	if (slot >= kMaxBindSlots)
		return empty;
	return g_uniformBindings[slot];
}

void SetVertexBufferBinding(uint32_t slot, const BufferBinding& binding)
{
	if (slot >= kMaxBindSlots)
		return;
	g_vertexBufferBindings[slot] = binding;
}

const BufferBinding& GetVertexBufferBinding(uint32_t slot)
{
	static const BufferBinding empty{};
	if (slot >= kMaxBindSlots)
		return empty;
	return g_vertexBufferBindings[slot];
}

void SetTextureBinding(uint32_t slot, const TextureBinding& binding)
{
	if (slot >= kMaxBindSlots)
		return;
	g_textureBindings[slot] = binding;
}

const TextureBinding& GetTextureBinding(uint32_t slot)
{
	static const TextureBinding empty{};
	if (slot >= kMaxBindSlots)
		return empty;
	return g_textureBindings[slot];
}

void ClearBindings()
{
	for (auto& binding : g_uniformBindings)
		binding = BufferBinding{};
	for (auto& binding : g_vertexBufferBindings)
		binding = BufferBinding{};
	for (auto& binding : g_textureBindings)
		binding = TextureBinding{};
}

void SetCurrentPipelineState(void* pipelineState) { g_currentPipelineState = pipelineState; }
void* GetCurrentPipelineState() { return g_currentPipelineState; }

void SetCurrentDrawable(void* drawable) { g_currentDrawable = drawable; }
void* GetCurrentDrawable() { return g_currentDrawable; }

void SetCurrentCommandBuffer(void* commandBuffer) { g_currentCommandBuffer = commandBuffer; }
void* GetCurrentCommandBuffer() { return g_currentCommandBuffer; }

} // namespace MetalGlobals
