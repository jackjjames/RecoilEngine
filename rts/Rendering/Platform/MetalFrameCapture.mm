/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Rendering/Platform/MetalFrameCapture.h"

#include "Rendering/GlobalRendering.h"
#include "Rendering/Textures/Bitmap.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

namespace {

const char* GetDumpDir()
{
	if (const char* dir = std::getenv("SPRING_METAL_FRAME_DUMP_DIR")) {
		if (dir[0] != '\0')
			return dir;
	}
	if (const char* dir = std::getenv("SPRING_METAL_SCREENSHOT_DIR")) {
		if (dir[0] != '\0')
			return dir;
	}
	return nullptr;
}

int GetDumpInterval()
{
	const char* value = std::getenv("SPRING_METAL_FRAME_DUMP_INTERVAL");
	if (value == nullptr)
		value = std::getenv("SPRING_METAL_SCREENSHOT_INTERVAL");

	return std::max(1, value != nullptr ? std::atoi(value) : 120);
}

bool ShouldDumpThisFrame()
{
	if (globalRendering == nullptr)
		return false;

	const unsigned interval = static_cast<unsigned>(GetDumpInterval());
	return (globalRendering->drawFrame % interval) == 0;
}

} // namespace

namespace MetalFrameCapture {

bool WantsFrameDump()
{
	return GetDumpDir() != nullptr;
}

void MaybeDumpDrawable(void* drawablePtr)
{
	if (!WantsFrameDump() || !ShouldDumpThisFrame())
		return;

	auto drawable = (__bridge id<CAMetalDrawable>)drawablePtr;
	if (drawable == nil || drawable.texture == nil)
		return;

	id<MTLTexture> texture = drawable.texture;
	const int width = static_cast<int>(texture.width);
	const int height = static_cast<int>(texture.height);
	if (width <= 0 || height <= 0)
		return;

	const char* dumpDir = GetDumpDir();
	std::error_code ec;
	std::filesystem::create_directories(dumpDir, ec);

	const size_t bytesPerRow = static_cast<size_t>(width) * 4;
	std::vector<uint8_t> bgra(bytesPerRow * static_cast<size_t>(height));
	[texture getBytes:bgra.data()
	      bytesPerRow:bytesPerRow
	       fromRegion:MTLRegionMake2D(0, 0, width, height)
	      mipmapLevel:0];

	std::vector<uint8_t> rgba(bgra.size());
	for (size_t i = 0; i < bgra.size(); i += 4) {
		rgba[i + 0] = bgra[i + 2];
		rgba[i + 1] = bgra[i + 1];
		rgba[i + 2] = bgra[i + 0];
		rgba[i + 3] = bgra[i + 3];
	}

	const std::string filename = fmt::format(
		"{}/metal_frame_{:06}.png",
		dumpDir,
		globalRendering->drawFrame);
	CBitmap bitmap(rgba.data(), width, height);
	bitmap.Save(filename, true, true, 90);
}

} // namespace MetalFrameCapture
