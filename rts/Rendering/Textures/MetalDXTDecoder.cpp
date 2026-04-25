/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/Textures/MetalDXTDecoder.h"

namespace MetalDXT {

namespace {

inline void Unpack565(uint16_t c, uint8_t out[4])
{
	const uint32_t r5 = (c >> 11) & 0x1F;
	const uint32_t g6 = (c >>  5) & 0x3F;
	const uint32_t b5 = (c      ) & 0x1F;
	out[0] = static_cast<uint8_t>((r5 * 255 + 15) / 31);
	out[1] = static_cast<uint8_t>((g6 * 255 + 31) / 63);
	out[2] = static_cast<uint8_t>((b5 * 255 + 15) / 31);
	out[3] = 255;
}

// Decode the 4x4 colour block shared by BC1/2/3. punchThroughAlpha controls
// the BC1 "1-bit alpha" mode: when c0 <= c1 the third palette entry blends
// 50/50 and the fourth becomes transparent. BC2/3 disable this since alpha
// is carried in a separate block.
void DecodeColorBlock(const uint8_t* src, uint8_t out[4][4][4], bool punchThroughAlpha)
{
	const uint16_t c0 = static_cast<uint16_t>(src[0] | (src[1] << 8));
	const uint16_t c1 = static_cast<uint16_t>(src[2] | (src[3] << 8));

	uint8_t palette[4][4];
	Unpack565(c0, palette[0]);
	Unpack565(c1, palette[1]);

	if (c0 > c1 || !punchThroughAlpha) {
		for (int k = 0; k < 3; ++k) {
			palette[2][k] = static_cast<uint8_t>((2 * palette[0][k] + palette[1][k]) / 3);
			palette[3][k] = static_cast<uint8_t>((palette[0][k] + 2 * palette[1][k]) / 3);
		}
		palette[2][3] = 255;
		palette[3][3] = 255;
	} else {
		for (int k = 0; k < 3; ++k) {
			palette[2][k] = static_cast<uint8_t>((palette[0][k] + palette[1][k]) / 2);
			palette[3][k] = 0;
		}
		palette[2][3] = 255;
		palette[3][3] = 0;
	}

	const uint32_t indices = static_cast<uint32_t>(src[4])
	                      | (static_cast<uint32_t>(src[5]) <<  8)
	                      | (static_cast<uint32_t>(src[6]) << 16)
	                      | (static_cast<uint32_t>(src[7]) << 24);

	for (int y = 0; y < 4; ++y) {
		for (int x = 0; x < 4; ++x) {
			const uint32_t bit = (indices >> (2 * (4 * y + x))) & 0x3u;
			out[y][x][0] = palette[bit][0];
			out[y][x][1] = palette[bit][1];
			out[y][x][2] = palette[bit][2];
			out[y][x][3] = palette[bit][3];
		}
	}
}

// BC3 alpha block: two 8-bit endpoints + 16 3-bit indices into a 6- or
// 8-entry interpolated palette. Returns alphas[y][x] in [0,255].
void DecodeBC3AlphaBlock(const uint8_t* src, uint8_t alphas[4][4])
{
	const uint8_t a0 = src[0];
	const uint8_t a1 = src[1];
	uint8_t pal[8];
	pal[0] = a0;
	pal[1] = a1;
	if (a0 > a1) {
		for (int i = 1; i < 7; ++i)
			pal[i + 1] = static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7);
	} else {
		for (int i = 1; i < 5; ++i)
			pal[i + 1] = static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5);
		pal[6] = 0;
		pal[7] = 255;
	}

	uint64_t bits = 0;
	for (int i = 0; i < 6; ++i)
		bits |= static_cast<uint64_t>(src[2 + i]) << (8 * i);

	for (int y = 0; y < 4; ++y) {
		for (int x = 0; x < 4; ++x) {
			const uint32_t idx = static_cast<uint32_t>((bits >> (3 * (4 * y + x))) & 0x7u);
			alphas[y][x] = pal[idx];
		}
	}
}

void WriteBlockToImage(const uint8_t in[4][4][4], uint8_t* dst, int width, int bx, int by)
{
	const int x0 = bx * 4;
	const int y0 = by * 4;
	for (int dy = 0; dy < 4; ++dy) {
		uint8_t* row = dst + ((y0 + dy) * width + x0) * 4;
		for (int dx = 0; dx < 4; ++dx) {
			row[dx * 4 + 0] = in[dy][dx][0];
			row[dx * 4 + 1] = in[dy][dx][1];
			row[dx * 4 + 2] = in[dy][dx][2];
			row[dx * 4 + 3] = in[dy][dx][3];
		}
	}
}

} // namespace

void DecompressBC1Image(const uint8_t* src, uint8_t* dst, int width, int height)
{
	const int blocksX = (width  + 3) / 4;
	const int blocksY = (height + 3) / 4;
	for (int by = 0; by < blocksY; ++by) {
		for (int bx = 0; bx < blocksX; ++bx) {
			const uint8_t* blk = src + ((by * blocksX) + bx) * 8;
			uint8_t pixels[4][4][4];
			DecodeColorBlock(blk, pixels, /*punchThroughAlpha=*/true);
			WriteBlockToImage(pixels, dst, width, bx, by);
		}
	}
}

void DecompressBC2Image(const uint8_t* src, uint8_t* dst, int width, int height)
{
	const int blocksX = (width  + 3) / 4;
	const int blocksY = (height + 3) / 4;
	for (int by = 0; by < blocksY; ++by) {
		for (int bx = 0; bx < blocksX; ++bx) {
			const uint8_t* blk = src + ((by * blocksX) + bx) * 16;
			uint8_t pixels[4][4][4];
			DecodeColorBlock(blk + 8, pixels, /*punchThroughAlpha=*/false);
			for (int y = 0; y < 4; ++y) {
				const uint8_t lo = blk[y * 2 + 0];
				const uint8_t hi = blk[y * 2 + 1];
				const uint8_t a4[4] = {
					static_cast<uint8_t>((lo & 0x0F)),
					static_cast<uint8_t>((lo >> 4)  ),
					static_cast<uint8_t>((hi & 0x0F)),
					static_cast<uint8_t>((hi >> 4)  ),
				};
				for (int x = 0; x < 4; ++x)
					pixels[y][x][3] = static_cast<uint8_t>((a4[x] * 255 + 7) / 15);
			}
			WriteBlockToImage(pixels, dst, width, bx, by);
		}
	}
}

void DecompressBC3Image(const uint8_t* src, uint8_t* dst, int width, int height)
{
	const int blocksX = (width  + 3) / 4;
	const int blocksY = (height + 3) / 4;
	for (int by = 0; by < blocksY; ++by) {
		for (int bx = 0; bx < blocksX; ++bx) {
			const uint8_t* blk = src + ((by * blocksX) + bx) * 16;
			uint8_t pixels[4][4][4];
			DecodeColorBlock(blk + 8, pixels, /*punchThroughAlpha=*/false);
			uint8_t alphas[4][4];
			DecodeBC3AlphaBlock(blk, alphas);
			for (int y = 0; y < 4; ++y)
				for (int x = 0; x < 4; ++x)
					pixels[y][x][3] = alphas[y][x];
			WriteBlockToImage(pixels, dst, width, bx, by);
		}
	}
}

} // namespace MetalDXT

#endif // RENDER_BACKEND_METAL
