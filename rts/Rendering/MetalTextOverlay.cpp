/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalTextOverlay.h"

#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Rendering/Textures/ITexture.h"
#include "Rendering/Textures/TextureCreationParams.hpp"
#include "System/Log/ILog.h"
#include "System/type2.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <vector>

namespace {

using TextVertex = MetalTextOverlay::TextVertex;

constexpr uint32_t kGL_R8 = 0x8229;

constexpr int kGlyphW    = 5;
constexpr int kGlyphH    = 7;
constexpr int kFirstChar = 0x20;
constexpr int kLastChar  = 0x7F; // exclusive end == 128
constexpr int kNumGlyphs = kLastChar - kFirstChar;
constexpr int kAtlasW    = kNumGlyphs * kGlyphW;
constexpr int kAtlasH    = kGlyphH;

// 5x7 ASCII bitmap font, indexed by (char - 0x20). Each row is a uint8_t;
// bit 4 is the leftmost pixel, bit 0 is the rightmost. Unlisted characters
// render as blanks (all zero). Hand-rolled with enough coverage for load-
// progress strings; lowercase letters fold to uppercase at draw time.
constexpr std::array<std::array<uint8_t, kGlyphH>, kNumGlyphs> kFont = {{
	/* 0x20 ' ' */ {},
	/* 0x21 '!' */ {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100},
	/* 0x22 '"' */ {0b01010, 0b01010, 0b01010, 0b00000, 0b00000, 0b00000, 0b00000},
	/* 0x23 '#' */ {0b01010, 0b01010, 0b11111, 0b01010, 0b11111, 0b01010, 0b01010},
	/* 0x24 '$' */ {},
	/* 0x25 '%' */ {0b11001, 0b11010, 0b00100, 0b01000, 0b10011, 0b10011, 0b00000},
	/* 0x26 '&' */ {},
	/* 0x27 '\''*/ {0b00100, 0b00100, 0b00100, 0b00000, 0b00000, 0b00000, 0b00000},
	/* 0x28 '(' */ {0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010},
	/* 0x29 ')' */ {0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000},
	/* 0x2A '*' */ {0b00000, 0b00100, 0b10101, 0b01110, 0b10101, 0b00100, 0b00000},
	/* 0x2B '+' */ {0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000},
	/* 0x2C ',' */ {0b00000, 0b00000, 0b00000, 0b00000, 0b00100, 0b00100, 0b01000},
	/* 0x2D '-' */ {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000},
	/* 0x2E '.' */ {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00100, 0b00100},
	/* 0x2F '/' */ {0b00001, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b10000},
	/* 0x30 '0' */ {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110},
	/* 0x31 '1' */ {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b11111},
	/* 0x32 '2' */ {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111},
	/* 0x33 '3' */ {0b01110, 0b10001, 0b00001, 0b00110, 0b00001, 0b10001, 0b01110},
	/* 0x34 '4' */ {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010},
	/* 0x35 '5' */ {0b11111, 0b10000, 0b10000, 0b11110, 0b00001, 0b10001, 0b01110},
	/* 0x36 '6' */ {0b01110, 0b10001, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110},
	/* 0x37 '7' */ {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000},
	/* 0x38 '8' */ {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110},
	/* 0x39 '9' */ {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b10001, 0b01110},
	/* 0x3A ':' */ {0b00000, 0b00100, 0b00100, 0b00000, 0b00100, 0b00100, 0b00000},
	/* 0x3B ';' */ {0b00000, 0b00100, 0b00100, 0b00000, 0b00100, 0b00100, 0b01000},
	/* 0x3C '<' */ {0b00010, 0b00100, 0b01000, 0b10000, 0b01000, 0b00100, 0b00010},
	/* 0x3D '=' */ {0b00000, 0b00000, 0b11111, 0b00000, 0b11111, 0b00000, 0b00000},
	/* 0x3E '>' */ {0b01000, 0b00100, 0b00010, 0b00001, 0b00010, 0b00100, 0b01000},
	/* 0x3F '?' */ {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100},
	/* 0x40 '@' */ {},
	/* 0x41 'A' */ {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001},
	/* 0x42 'B' */ {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110},
	/* 0x43 'C' */ {0b01111, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b01111},
	/* 0x44 'D' */ {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110},
	/* 0x45 'E' */ {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111},
	/* 0x46 'F' */ {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000},
	/* 0x47 'G' */ {0b01111, 0b10000, 0b10000, 0b10011, 0b10001, 0b10001, 0b01111},
	/* 0x48 'H' */ {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001},
	/* 0x49 'I' */ {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b11111},
	/* 0x4A 'J' */ {0b01111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100},
	/* 0x4B 'K' */ {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001},
	/* 0x4C 'L' */ {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111},
	/* 0x4D 'M' */ {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001},
	/* 0x4E 'N' */ {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001},
	/* 0x4F 'O' */ {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110},
	/* 0x50 'P' */ {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000},
	/* 0x51 'Q' */ {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101},
	/* 0x52 'R' */ {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001},
	/* 0x53 'S' */ {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110},
	/* 0x54 'T' */ {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100},
	/* 0x55 'U' */ {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110},
	/* 0x56 'V' */ {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100},
	/* 0x57 'W' */ {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b11011, 0b10001},
	/* 0x58 'X' */ {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001},
	/* 0x59 'Y' */ {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100},
	/* 0x5A 'Z' */ {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111},
	/* 0x5B '[' */ {0b01110, 0b01000, 0b01000, 0b01000, 0b01000, 0b01000, 0b01110},
	/* 0x5C '\\'*/ {0b10000, 0b10000, 0b01000, 0b00100, 0b00010, 0b00001, 0b00001},
	/* 0x5D ']' */ {0b01110, 0b00010, 0b00010, 0b00010, 0b00010, 0b00010, 0b01110},
	/* 0x5E '^' */ {0b00100, 0b01010, 0b10001, 0b00000, 0b00000, 0b00000, 0b00000},
	/* 0x5F '_' */ {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b11111},
	/* 0x60 '`' */ {0b01000, 0b00100, 0b00010, 0b00000, 0b00000, 0b00000, 0b00000},
	// 0x61..0x7A lowercase -> fold to uppercase at draw time; leave blank.
	/* 0x61 'a' */ {}, /* 0x62 'b' */ {}, /* 0x63 'c' */ {}, /* 0x64 'd' */ {},
	/* 0x65 'e' */ {}, /* 0x66 'f' */ {}, /* 0x67 'g' */ {}, /* 0x68 'h' */ {},
	/* 0x69 'i' */ {}, /* 0x6A 'j' */ {}, /* 0x6B 'k' */ {}, /* 0x6C 'l' */ {},
	/* 0x6D 'm' */ {}, /* 0x6E 'n' */ {}, /* 0x6F 'o' */ {}, /* 0x70 'p' */ {},
	/* 0x71 'q' */ {}, /* 0x72 'r' */ {}, /* 0x73 's' */ {}, /* 0x74 't' */ {},
	/* 0x75 'u' */ {}, /* 0x76 'v' */ {}, /* 0x77 'w' */ {}, /* 0x78 'x' */ {},
	/* 0x79 'y' */ {}, /* 0x7A 'z' */ {},
	/* 0x7B '{' */ {0b00010, 0b00100, 0b00100, 0b01000, 0b00100, 0b00100, 0b00010},
	/* 0x7C '|' */ {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100},
	/* 0x7D '}' */ {0b01000, 0b00100, 0b00100, 0b00010, 0b00100, 0b00100, 0b01000},
	/* 0x7E '~' */ {0b00000, 0b00000, 0b01001, 0b10110, 0b00000, 0b00000, 0b00000},
}};

static_assert(kFont.size() == kNumGlyphs, "font table size mismatch");

constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 0) out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Single-channel (R8) atlas: any non-zero coverage draws as opaque white, the
// rest discards. No alpha blending required which keeps the pipeline state in
// lock-step with MetalSplashRenderer (no blend attachments yet).
constexpr const char* kFragmentGlsl = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;
layout(set = 0, binding = 0) uniform sampler2D uTex;
void main() {
    float c = texture(uTex, vUV).r;
    if (c < 0.5) discard;
    fragColor = vec4(1.0, 1.0, 1.0, 1.0);
}
)";

void BuildAtlasPixels(std::vector<uint8_t>& out)
{
	out.assign(kAtlasW * kAtlasH, 0);
	for (int g = 0; g < kNumGlyphs; ++g) {
		const auto& glyph = kFont[g];
		const int baseCol = g * kGlyphW;
		for (int row = 0; row < kGlyphH; ++row) {
			const uint8_t bits = glyph[row];
			for (int col = 0; col < kGlyphW; ++col) {
				// bit 4 is the leftmost pixel.
				const bool on = (bits >> (kGlyphW - 1 - col)) & 1;
				out[row * kAtlasW + baseCol + col] = on ? 0xFF : 0x00;
			}
		}
	}
}

} // namespace


MetalTextOverlay::MetalTextOverlay()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;

	auto& backend = *globalRendering->renderBackend;

	// --- Atlas texture (R8, nearest filtering for crisp bitmap pixels).
	std::vector<uint8_t> pixels;
	BuildAtlasPixels(pixels);

	GL::TextureCreationParams tcp;
	tcp.linearTextureFilter = false;
	tcp.linearMipMapFilter  = false;
	tcp.reqNumLevels = 1;
	atlas = backend.CreateTexture2D(int2(kAtlasW, kAtlasH), kGL_R8, tcp, /*wantCompress=*/false);
	if (!atlas || !atlas->IsValid())
		return;
	atlas->UploadImage(pixels.data());

	// --- Vertex buffers (dynamic, 6 verts per glyph). Rotate between a few
	// frame slots and append per draw so command-buffered Metal draws do not
	// observe later text uploads into the same byte range.
	constexpr size_t kInitialGlyphCap = 4096;
	for (size_t i = 0; i < vertexBuffers.size(); ++i) {
		vertexBufferCapacityBytes[i] = kInitialGlyphCap * 6 * sizeof(TextVertex);
		vertexBuffers[i] = backend.CreateBuffer(vertexBufferCapacityBytes[i], nullptr);
		if (!vertexBuffers[i] || !vertexBuffers[i]->IsValid())
			return;
	}

	// --- Pipeline. Interleaved vec2 pos + vec2 uv, same layout as the
	// splash quad so the shader translator + Metal pipeline cache treat
	// them symmetrically.
	PipelineDesc pd;
	pd.name = "text_overlay";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	pd.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = 0,                 .format = VertexFormat::Float2 },
		VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = sizeof(float) * 2, .format = VertexFormat::Float2 },
	};
	pd.vertexBindings = {
		VertexBindingLayout{ .slot = 0, .stride = sizeof(TextVertex) },
	};

	pipeline = backend.CreatePipeline(pd);
	if (!pipeline || !pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalTextOverlay] pipeline did not link");
		return;
	}

	valid = true;
}

MetalTextOverlay::~MetalTextOverlay() = default;


void MetalTextOverlay::BeginFrame()
{
	activeVertexBuffer = (activeVertexBuffer + 1) % vertexBuffers.size();
	vertexBufferOffsetBytes = 0;
}


void MetalTextOverlay::DrawLine(float ndcX, float ndcY, float ndcGlyphH, const std::string& text,
                                float ndcMaxX)
{
	if (!valid || text.empty())
		return;

	// Preserve the on-screen 5:7 glyph aspect. NDC spans [-1, 1] = 2 units
	// over viewportH vertically and 2 units over viewportW horizontally, so
	// dividing by aspectRatio gives us a width whose pixel extent matches
	// the font's intended shape regardless of window size.
	const float aspect = globalRendering != nullptr && globalRendering->aspectRatio > 0.0f
		? globalRendering->aspectRatio : 1.0f;
	float ndcGlyphW = ndcGlyphH * (float(kGlyphW) / float(kGlyphH)) / aspect;
	// Inter-glyph advance: glyph width plus a small gap for readability.
	constexpr float kAdvanceRatio = 1.2f;
	float advance = ndcGlyphW * kAdvanceRatio;

	// Auto-shrink so the whole line fits inside [ndcX, ndcMaxX]. `advance`
	// is per-character; the last glyph adds its own width on top which we
	// approximate as one extra advance's worth for simplicity.
	const float available = ndcMaxX - ndcX;
	if (available > 0.0f) {
		const float needed = advance * float(text.size());
		if (needed > available) {
			const float s = available / needed;
			ndcGlyphW *= s;
			advance   *= s;
		}
	}

	// NDC y grows upward; we treat (ndcX, ndcY) as the top-left of the
	// text baseline cell and emit glyphs downward from there.
	const float yTop    = ndcY;
	const float yBottom = ndcY - ndcGlyphH;

	constexpr float kAtlasWf = float(kAtlasW);
	constexpr float kGlyphUW = float(kGlyphW) / float(kAtlasW);

	vertsScratch.clear();
	vertsScratch.reserve(text.size() * 6);

	float cursorX = ndcX;
	for (char raw : text) {
		unsigned char ch = static_cast<unsigned char>(raw);
		if (ch >= 'a' && ch <= 'z')
			ch = ch - 'a' + 'A';
		if (ch < kFirstChar || ch >= kLastChar) {
			cursorX += advance;
			continue;
		}
		const int glyphIdx = int(ch) - kFirstChar;

		bool any = false;
		for (uint8_t row : kFont[glyphIdx]) {
			if (row != 0) { any = true; break; }
		}
		if (!any) {
			cursorX += advance;
			continue;
		}

		const float x0 = cursorX;
		const float x1 = cursorX + ndcGlyphW;
		const float u0 = (glyphIdx * kGlyphW) / kAtlasWf;
		const float u1 = u0 + kGlyphUW;
		// Atlas is uploaded with row 0 at the top (matches CBitmap ordering
		// for the splash path); sample v=0 at top and v=1 at bottom.
		constexpr float v0 = 0.0f;
		constexpr float v1 = 1.0f;

		vertsScratch.push_back({x0, yBottom, u0, v1});
		vertsScratch.push_back({x1, yBottom, u1, v1});
		vertsScratch.push_back({x0, yTop,    u0, v0});
		vertsScratch.push_back({x0, yTop,    u0, v0});
		vertsScratch.push_back({x1, yBottom, u1, v1});
		vertsScratch.push_back({x1, yTop,    u1, v0});

		cursorX += advance;
	}

	if (vertsScratch.empty())
		return;

	const size_t bytes = vertsScratch.size() * sizeof(TextVertex);
	if (vertexBufferOffsetBytes + bytes > vertexBufferCapacityBytes[activeVertexBuffer]) {
		auto& backend = *globalRendering->renderBackend;
		vertexBufferCapacityBytes[activeVertexBuffer] = std::max(vertexBufferCapacityBytes[activeVertexBuffer] * 2, bytes * 2);
		vertexBuffers[activeVertexBuffer] = backend.CreateBuffer(vertexBufferCapacityBytes[activeVertexBuffer], nullptr);
		if (!vertexBuffers[activeVertexBuffer] || !vertexBuffers[activeVertexBuffer]->IsValid())
			return;
		vertexBufferOffsetBytes = 0;
	}

	const size_t drawOffset = vertexBufferOffsetBytes;
	vertexBuffers[activeVertexBuffer]->UpdateData(vertsScratch.data(), bytes, drawOffset);
	vertexBufferOffsetBytes += bytes;

	pipeline->Enable();
	pipeline->BindVertexBuffer(0, *vertexBuffers[activeVertexBuffer], drawOffset);
	pipeline->BindTexture(0, *atlas);
	pipeline->Draw(PrimitiveTopology::Triangles, 0, static_cast<uint32_t>(vertsScratch.size()));
	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
