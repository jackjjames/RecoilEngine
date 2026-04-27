/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

namespace MetalFrameCapture {

// Debug-only frame dump hook for Metal. Enabled by setting
// SPRING_METAL_FRAME_DUMP_DIR to an output directory. The legacy
// SPRING_METAL_SCREENSHOT_DIR name is also accepted while the port is in flux.
bool WantsFrameDump();

// drawable is id<CAMetalDrawable>, passed as void* to keep this header ObjC-free.
void MaybeDumpDrawable(void* drawable);

} // namespace MetalFrameCapture
