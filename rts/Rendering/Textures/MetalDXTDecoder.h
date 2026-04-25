/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <cstddef>
#include <cstdint>

// CPU decoders for DXT1/3/5 (BC1/2/3) -> RGBA8. Used to bring DDS-compressed
// textures (S3O models, SMF minimaps, feature decals) onto the Metal backend
// without requiring MTLPixelFormatBC* support, which is missing on Apple
// Silicon Macs. The replacement-region path on Metal still wants RGBA8 so we
// always decode CPU-side. When proper BC support is wired (Intel discrete +
// future), these decoders go away.

namespace MetalDXT {

// Block-compressed payload byte count for a DXTn image of the given pixel
// dimensions. width/height are rounded up to the nearest multiple of 4.
constexpr size_t BC1ByteSize(int width, int height) {
	const int bx = (width  + 3) / 4;
	const int by = (height + 3) / 4;
	return static_cast<size_t>(bx) * by * 8; // 8 bytes/block
}
constexpr size_t BC2ByteSize(int width, int height) {
	const int bx = (width  + 3) / 4;
	const int by = (height + 3) / 4;
	return static_cast<size_t>(bx) * by * 16; // 16 bytes/block
}
constexpr size_t BC3ByteSize(int width, int height) {
	return BC2ByteSize(width, height); // also 16 bytes/block
}

// Decompress a BC1 (DXT1) image to RGBA8. width/height should be multiples
// of 4; non-multiples emit garbage in the trailing pixels.
void DecompressBC1Image(const uint8_t* src, uint8_t* dst, int width, int height);

// Decompress a BC2 (DXT3) image to RGBA8 (explicit 4-bit alpha).
void DecompressBC2Image(const uint8_t* src, uint8_t* dst, int width, int height);

// Decompress a BC3 (DXT5) image to RGBA8 (interpolated 8-bit alpha).
void DecompressBC3Image(const uint8_t* src, uint8_t* dst, int width, int height);

} // namespace MetalDXT

#endif // RENDER_BACKEND_METAL
