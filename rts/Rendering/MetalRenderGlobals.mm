/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Rendering/MetalRenderGlobals.h"

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

} // namespace

void SetDevice(void* device) { g_device = device; }
void* GetDevice() { return g_device; }

void SetCommandQueue(void* queue) { g_commandQueue = queue; }
void* GetCommandQueue() { return g_commandQueue; }

void SetLayer(void* layer) { g_layer = layer; }
void* GetLayer() { return g_layer; }

void SetCurrentEncoder(void* encoder) { g_currentEncoder = encoder; }
void* GetCurrentEncoder() { return g_currentEncoder; }

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

void ClearBindings()
{
	for (auto& binding : g_uniformBindings)
		binding = BufferBinding{};
}

void SetCurrentPipelineState(void* pipelineState) { g_currentPipelineState = pipelineState; }
void* GetCurrentPipelineState() { return g_currentPipelineState; }

void SetCurrentDrawable(void* drawable) { g_currentDrawable = drawable; }
void* GetCurrentDrawable() { return g_currentDrawable; }

void SetCurrentCommandBuffer(void* commandBuffer) { g_currentCommandBuffer = commandBuffer; }
void* GetCurrentCommandBuffer() { return g_currentCommandBuffer; }

} // namespace MetalGlobals
