/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalLuaUIRenderer.h"

#include "Rendering/GlobalRendering.h"
#include "Rendering/Fonts/glFont.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/IRenderTarget.h"
#include "Rendering/MetalRenderGlobals.h"
#include "Rendering/MetalTextOverlay.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Rendering/Textures/Bitmap.h"
#include "Rendering/Textures/ITexture.h"
#include "Rendering/Textures/TextureCreationParams.hpp"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitDefHandler.h"
#include "System/Log/ILog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Affine2D {
	float a = 1.0f, b = 0.0f;
	float c = 0.0f, d = 1.0f;
	float tx = 0.0f, ty = 0.0f;

	std::pair<float, float> Transform(float x, float y) const
	{
		return {a * x + c * y + tx, b * x + d * y + ty};
	}

	void PostTranslate(float x, float y)
	{
		tx += a * x + c * y;
		ty += b * x + d * y;
	}

	void PostScale(float x, float y)
	{
		a *= x; b *= x;
		c *= y; d *= y;
	}

	void PostRotate(float degrees)
	{
		constexpr float degToRad = 3.14159265358979323846f / 180.0f;
		const float radians = degrees * degToRad;
		const float sinAngle = std::sin(radians);
		const float cosAngle = std::cos(radians);
		const float nextA = a * cosAngle + c * sinAngle;
		const float nextB = b * cosAngle + d * sinAngle;
		const float nextC = c * cosAngle - a * sinAngle;
		const float nextD = d * cosAngle - b * sinAngle;
		a = nextA;
		b = nextB;
		c = nextC;
		d = nextD;
	}
};

struct CapturedText {
	LuaUITextDraw draw;
	float localX = 0.0f;
	float localY = 0.0f;
	float localSize = 0.0f;
};

struct TextureCommandBuffer {
	LuaUITextureDesc desc;
	std::unique_ptr<ITexture> texture;
	std::vector<uint32_t> pixels;
	std::vector<CapturedText> texts;
	// Cache flag toggled by the lazy GPU upload in DrawTextureScreenQuad,
	// hence mutable.
	mutable bool dirty = false;
};

struct ListCommand {
	enum class Type {
		Text,
		Rect,
		Triangle,
		Texture,
		TextureTriangle,
		BindTexture,
		Color,
		BlendState,
		PushMatrix,
		PopMatrix,
		LoadIdentity,
		Ortho,
		Translate,
		Rotate,
		Scale,
	};

	Type type = Type::Rect;
	LuaUITextDraw text;
	LuaUIRectDraw rect;
	int textureID = 0;
	float x1 = 0.0f;
	float y1 = 0.0f;
	float x2 = 0.0f;
	float y2 = 0.0f;
	float x3 = 0.0f;
	float y3 = 0.0f;
	float u1 = 0.0f;
	float v1 = 0.0f;
	float u2 = 1.0f;
	float v2 = 1.0f;
	float u3 = 1.0f;
	float v3 = 1.0f;
	float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	// Triangle commands keep per-vertex color so feather outlines and
	// gradient fills (RectRoundOutline / RectRound) interpolate correctly.
	float color2[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	float color3[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	bool boolValue = false;
	uint32_t srcColor = 0;
	uint32_t dstColor = 0;
	uint32_t srcAlpha = 0;
	uint32_t dstAlpha = 0;
};

struct RectVertex {
	float pos[2];
	float color[4];
};

struct TextureVertex {
	float pos[2];
	float uv[2];
	float color[4];
};

constexpr uint32_t kGL_RGBA8 = 0x8058;

constexpr const char* kRectVertexGlsl = R"(#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 0) out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

constexpr const char* kRectFragmentGlsl = R"(#version 450
layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 fragColor;
void main() {
    fragColor = vColor;
}
)";

constexpr const char* kTextureVertexGlsl = R"(#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

constexpr const char* kTextureFragmentGlsl = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 fragColor;
layout(set = 0, binding = 0) uniform sampler2D uTex;
void main() {
    fragColor = vec4(vColor.rgb, 1.0);
}
)";

std::string StripColorCodes(const std::string& text)
{
	std::string stripped;
	stripped.reserve(text.size());

	for (size_t i = 0; i < text.size(); ++i) {
		const unsigned char ch = static_cast<unsigned char>(text[i]);
		if (ch == 0xFF && i + 3 < text.size()) {
			i += 3;
			continue;
		}
		if (ch >= 0x20 && ch < 0x7F)
			stripped.push_back(char(ch));
	}

	return stripped;
}

std::string NormalizeNamedTexturePath(const std::string& name)
{
	if (name.size() < 3 || name[0] != ':')
		return name;

	size_t secondColon = name.find(':', 1);
	if (secondColon == std::string::npos)
		return name;

	// Lua texture names can start with sampler hints like :n:, :l:, :g: or
	// tint hints like :lt0.3,0.3,0.3:. They are GL sampling/state requests;
	// the Metal LuaUI fallback only needs the underlying VFS path.
	if (secondColon + 1 < name.size())
		return name.substr(secondColon + 1);

	return {};
}

class Renderer
{
public:
	void Init()
	{
		if (valid || globalRendering == nullptr || globalRendering->renderBackend == nullptr)
			return;

		textOverlay = std::make_unique<MetalTextOverlay>();
		if (!textOverlay->IsValid())
			return;

		auto& backend = *globalRendering->renderBackend;

		constexpr size_t rectVertexCapacity = 6 * sizeof(RectVertex);
		rectVertexBuffer = backend.CreateBuffer(rectVertexCapacity, nullptr);
		if (!rectVertexBuffer || !rectVertexBuffer->IsValid())
			return;

		textureVertexBuffer = backend.CreateBuffer(6 * sizeof(TextureVertex), nullptr);
		if (!textureVertexBuffer || !textureVertexBuffer->IsValid())
			return;

		PipelineDesc rectDesc;
		rectDesc.name = "lua_ui_rect";
		rectDesc.vertexSource = kRectVertexGlsl;
		rectDesc.fragmentSource = kRectFragmentGlsl;
		rectDesc.vertexAttributes = {
			VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = 0, .format = VertexFormat::Float2 },
			VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = sizeof(float) * 2, .format = VertexFormat::Float4 },
		};
		rectDesc.vertexBindings = {
			VertexBindingLayout{ .slot = 0, .stride = sizeof(RectVertex) },
		};
		rectDesc.blendState = RenderTargetBlendState{
			.enabled = true,
			.srcColor = GL_SRC_ALPHA,
			.dstColor = GL_ONE_MINUS_SRC_ALPHA,
			.srcAlpha = GL_ONE,
			.dstAlpha = GL_ONE_MINUS_SRC_ALPHA,
		};
		rectPipeline = backend.CreatePipeline(rectDesc);
		if (!rectPipeline || !rectPipeline->IsValid())
			return;

		PipelineDesc textureDesc;
		textureDesc.name = "lua_ui_texture";
		textureDesc.vertexSource = kTextureVertexGlsl;
		textureDesc.fragmentSource = kTextureFragmentGlsl;
		textureDesc.vertexAttributes = {
			VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = 0, .format = VertexFormat::Float2 },
			VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = sizeof(float) * 2, .format = VertexFormat::Float2 },
			VertexAttribute{ .location = 2, .bufferSlot = 0, .offset = sizeof(float) * 4, .format = VertexFormat::Float4 },
		};
		textureDesc.vertexBindings = {
			VertexBindingLayout{ .slot = 0, .stride = sizeof(TextureVertex) },
		};
		textureDesc.blendState = rectDesc.blendState;
		texturePipeline = backend.CreatePipeline(textureDesc);
		textureDesc.name = "lua_ui_texture_premultiplied";
		textureDesc.blendState = RenderTargetBlendState{
			.enabled = true,
			.srcColor = GL_ONE,
			.dstColor = GL_ONE_MINUS_SRC_ALPHA,
			.srcAlpha = GL_ONE,
			.dstAlpha = GL_ONE_MINUS_SRC_ALPHA,
		};
		texturePremultipliedPipeline = backend.CreatePipeline(textureDesc);
		textureDesc.name = "lua_ui_texture_additive";
		textureDesc.blendState = RenderTargetBlendState{
			.enabled = true,
			.srcColor = GL_SRC_ALPHA,
			.dstColor = GL_ONE,
			.srcAlpha = GL_ONE,
			.dstAlpha = GL_ONE,
		};
		textureAdditivePipeline = backend.CreatePipeline(textureDesc);
		valid = texturePipeline && texturePipeline->IsValid() &&
		        texturePremultipliedPipeline && texturePremultipliedPipeline->IsValid() &&
		        textureAdditivePipeline && textureAdditivePipeline->IsValid();
	}

	void Kill()
	{
		textures.clear();
		matrixStack.clear();
		capturingTexture = 0;
		capturingList = 0;
		boundTexture = 0;
		nextTextureID = 1;
		nextListID = 1;
		namedTextureIDs.clear();
		scissorEnabled = false;
		blendEnabled = true;
		blendSrcColor = GL_SRC_ALPHA;
		blendDstColor = GL_ONE_MINUS_SRC_ALPHA;
		blendSrcAlpha = GL_ONE;
		blendDstAlpha = GL_ONE_MINUS_SRC_ALPHA;
		texturePipeline.reset();
		texturePremultipliedPipeline.reset();
		textureAdditivePipeline.reset();
		rectPipeline.reset();
		textureVertexBuffer.reset();
		rectVertexBuffer.reset();
		textOverlay.reset();
		valid = false;
	}

	void BeginFrame()
	{
		if (textOverlay != nullptr)
			textOverlay->BeginFrame();
		matrixStack.clear();
		matrixStack.push_back(Affine2D{});
		color = {1.0f, 1.0f, 1.0f, 1.0f};
		boundTexture = 0;
		capturingTexture = 0;
	}

	int CreateTexture(int width, int height)
	{
		const int textureID = nextTextureID++;
		auto& texture = textures[textureID];
		texture.desc.width = std::max(1, width);
		texture.desc.height = std::max(1, height);
		texture.pixels.resize(texture.desc.width * texture.desc.height, 0);
		texture.dirty = true;

		GL::TextureCreationParams tcp;
		tcp.linearTextureFilter = true;
		tcp.linearMipMapFilter = false;
		tcp.reqNumLevels = 1;
		texture.texture = globalRendering->renderBackend->CreateTexture2D(
			int2(texture.desc.width, texture.desc.height), kGL_RGBA8, tcp, false);
		if (texture.texture != nullptr && texture.texture->IsValid())
			texture.texture->UploadImage(texture.pixels.data());
		return textureID;
	}

	void DeleteTexture(int textureID)
	{
		textures.erase(textureID);
		if (boundTexture == textureID)
			boundTexture = 0;
	}

	void BindTexture(int textureID)
	{
		boundTexture = textureID;
		RecordTextureBind(textureID);
	}

	bool BindBitmapTexture(const std::string& cacheKey, CBitmap& bitmap)
	{
		GL::TextureCreationParams tcp;
		tcp.linearTextureFilter = true;
		tcp.linearMipMapFilter = false;
		tcp.reqNumLevels = 1;

		if (globalRendering == nullptr || globalRendering->renderBackend == nullptr) {
			UnbindTexture();
			return false;
		}

		const int textureID = nextTextureID++;
		auto& texture = textures[textureID];
		texture.desc.width = std::max(1, bitmap.xsize);
		texture.desc.height = std::max(1, bitmap.ysize);

		const size_t pxCount = size_t(texture.desc.width) * size_t(texture.desc.height);
		texture.pixels.resize(pxCount);

		const uint8_t* src = bitmap.GetRawMem();
		const size_t memSize = bitmap.GetMemSize();
		// DevIL may leave `dataType` as IL_* values; GL::GetDataTypeSize only knows GL_* enums.
		// Derive bytes-per-pixel from the allocated slab so RGBA LuaUI icons still populate
		// `pixels` for software R2T (otherwise RasterizeTextureRect sees empty source → no icons).
		const size_t bpp = (src != nullptr && pxCount > 0 && memSize >= pxCount) ? (memSize / pxCount) : 0;

		if (bpp == 4) {
			std::memcpy(texture.pixels.data(), src, pxCount * 4);
		} else if (bpp == 3) {
			const uint8_t* p = src;
			uint32_t* dst = texture.pixels.data();
			for (size_t i = 0; i < pxCount; ++i) {
				dst[i] = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (255u << 24);
				p += 3;
			}
		} else if (bpp == 2) {
			const uint8_t* p = src;
			uint32_t* dst = texture.pixels.data();
			for (size_t i = 0; i < pxCount; ++i) {
				const uint8_t lum = p[0];
				const uint8_t a = p[1];
				dst[i] = uint32_t(lum) | (uint32_t(lum) << 8) | (uint32_t(lum) << 16) | (uint32_t(a) << 24);
				p += 2;
			}
		} else if (bpp == 1) {
			const uint8_t* p = src;
			uint32_t* dst = texture.pixels.data();
			for (size_t i = 0; i < pxCount; ++i) {
				const uint8_t lum = p[0];
				dst[i] = uint32_t(lum) | (uint32_t(lum) << 8) | (uint32_t(lum) << 16) | (255u << 24);
				p += 1;
			}
		} else if (bpp == 16) {
			const float* p = reinterpret_cast<const float*>(src);
			uint32_t* dst = texture.pixels.data();
			const auto clampByte = [](float v) {
				return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
			};
			for (size_t i = 0; i < pxCount; ++i) {
				dst[i] = clampByte(p[0]) | (clampByte(p[1]) << 8) | (clampByte(p[2]) << 16) | (clampByte(p[3]) << 24);
				p += 4;
			}
		} else {
			std::fill(texture.pixels.begin(), texture.pixels.end(), 0);
		}

		// Always use an RGBA8 Metal texture for LuaUI bitmaps. CBitmap::CreateTextureHandle
		// keeps GL_RGB8 for 3-channel images; MetalTexture::UploadImage skips RGB8 uploads,
		// so DrawTextureScreenQuad never pushed icon pixels to the GPU (RGB PNG icons vanished).
		texture.texture = globalRendering->renderBackend->CreateTexture2D(
			int2(texture.desc.width, texture.desc.height), kGL_RGBA8, tcp, false);
		if (texture.texture == nullptr || !texture.texture->IsValid()) {
			textures.erase(textureID);
			UnbindTexture();
			return false;
		}
		texture.texture->UploadImage(texture.pixels.data());
		texture.dirty = false;

		namedTextureIDs[cacheKey] = textureID;
		boundTexture = textureID;
		RecordTextureBind(textureID);
		return true;
	}

	bool BindUnitDefBuildPic(const std::string& name)
	{
		if (name.size() < 2 || name[0] != '#' || unitDefHandler == nullptr)
			return false;

		char* endPtr = nullptr;
		const int unitDefID = static_cast<int>(std::strtol(name.c_str() + 1, &endPtr, 10));
		if (endPtr == name.c_str() + 1)
			return false;

		const UnitDef* unitDef = unitDefHandler->GetUnitDefByID(unitDefID);
		if (unitDef == nullptr)
			return false;

		CBitmap bitmap;
		if (!unitDef->buildPicName.empty()) {
			if (!bitmap.Load("unitpics/" + unitDef->buildPicName))
				return false;
		} else if (!bitmap.Load("unitpics/" + unitDef->name + ".dds") &&
		           !bitmap.Load("unitpics/" + unitDef->name + ".png") &&
		           !bitmap.Load("unitpics/" + unitDef->name + ".pcx") &&
		           !bitmap.Load("unitpics/" + unitDef->name + ".bmp")) {
			return false;
		}

		return BindBitmapTexture(name, bitmap);
	}

	void BindNamedTexture(const std::string& name)
	{
		if (name.empty()) {
			UnbindTexture();
			return;
		}

		const std::string texturePath = NormalizeNamedTexturePath(name);
		if (texturePath.empty()) {
			UnbindTexture();
			return;
		}

		const auto existing = namedTextureIDs.find(name);
		if (existing != namedTextureIDs.end()) {
			boundTexture = existing->second;
			RecordTextureBind(boundTexture);
			return;
		}

		if (BindUnitDefBuildPic(name))
			return;

		CBitmap bitmap;
		if (!bitmap.Load(texturePath)) {
			UnbindTexture();
			return;
		}

		BindBitmapTexture(name, bitmap);
	}
	void UnbindTexture()
	{
		boundTexture = 0;
		RecordTextureBind(0);
	}
	bool HasBoundTexture() const { return textures.find(boundTexture) != textures.end(); }

	void RenderToTexture(int textureID, const std::function<void()>& drawFunc)
	{
		auto it = textures.find(textureID);
		if (it == textures.end())
			return;

		// Real GL switches to a separate FBO with its own viewport / matrix /
		// scissor scope. Save and restore every Lua-visible draw-state knob
		// so widget-side gl.Scissor / gl.Color / gl.Blending / gl.Texture
		// calls inside the capture don't leak onto subsequent screen draws.
		const int previousCapture = capturingTexture;
		const int previousList = capturingList;
		const auto previousStack = matrixStack;
		const auto previousColor = color;
		const int previousBoundTexture = boundTexture;
		const bool previousBlendEnabled = blendEnabled;
		const uint32_t previousBlendSrcColor = blendSrcColor;
		const uint32_t previousBlendDstColor = blendDstColor;
		const uint32_t previousBlendSrcAlpha = blendSrcAlpha;
		const uint32_t previousBlendDstAlpha = blendDstAlpha;
		const bool previousScissorEnabled = scissorEnabled;
		const int previousScissorX = scissorX;
		const int previousScissorY = scissorY;
		const int previousScissorW = scissorW;
		const int previousScissorH = scissorH;

		capturingTexture = textureID;
		capturingList = 0;
		matrixStack.clear();
		matrixStack.push_back(Affine2D{});
		// Inside the FBO the widget supplies its own scissor coordinates in
		// texture-local pixels. Reset until the widget enables it explicitly.
		scissorEnabled = false;

		drawFunc();
		if (std::getenv("SPRING_METAL_LUAUI_DUMP_R2T") != nullptr && it->second.desc.width > 1000 && it->second.desc.height < 100) {
			static int dumpCount = 0;
			if (dumpCount++ < 6) {
				const std::string path = std::string(std::getenv("SPRING_METAL_LUAUI_DUMP_R2T")) +
					"/r2t_" + std::to_string(dumpCount) + "_id" + std::to_string(textureID) +
					"_" + std::to_string(it->second.desc.width) + "x" +
					std::to_string(it->second.desc.height) + ".png";
				CBitmap bitmap(reinterpret_cast<const uint8_t*>(it->second.pixels.data()),
				               it->second.desc.width, it->second.desc.height, 4);
				bitmap.Save(path, false, true);
			}
		}

		matrixStack = previousStack;
		color = previousColor;
		boundTexture = previousBoundTexture;
		blendEnabled = previousBlendEnabled;
		blendSrcColor = previousBlendSrcColor;
		blendDstColor = previousBlendDstColor;
		blendSrcAlpha = previousBlendSrcAlpha;
		blendDstAlpha = previousBlendDstAlpha;
		scissorEnabled = previousScissorEnabled;
		scissorX = previousScissorX;
		scissorY = previousScissorY;
		scissorW = previousScissorW;
		scissorH = previousScissorH;
		capturingList = previousList;
		capturingTexture = previousCapture;
	}

	void Clear(float r, float g, float b, float a)
	{
		auto it = textures.find(capturingTexture);
		if (it == textures.end())
			return;

		auto& texture = it->second;
		int x0 = 0;
		int y0 = 0;
		int x1 = texture.desc.width;
		int y1 = texture.desc.height;
		ApplyCaptureClip(x0, y0, x1, y1);
		if (x0 >= x1 || y0 >= y1)
			return;

		const float clearColor[4] = {r, g, b, a};
		const uint32_t packed = PackColor(clearColor);
		for (int y = y0; y < y1; ++y) {
			uint32_t* row = texture.pixels.data() + y * texture.desc.width;
			std::fill(row + x0, row + x1, packed);
		}

		if (x0 == 0 && y0 == 0 && x1 == texture.desc.width && y1 == texture.desc.height) {
			texture.texts.clear();
		} else {
			auto& texts = texture.texts;
			texts.erase(std::remove_if(texts.begin(), texts.end(), [&](const CapturedText& text) {
				return text.localX >= x0 && text.localX < x1 && text.localY >= y0 && text.localY < y1;
			}), texts.end());
		}

		texture.dirty = true;
	}

	bool GetCaptureTextureSize(int& width, int& height) const
	{
		if (capturingTexture == 0)
			return false;

		const auto it = textures.find(capturingTexture);
		if (it == textures.end())
			return false;

		width = it->second.desc.width;
		height = it->second.desc.height;
		return true;
	}

	bool IsCapturing() const { return capturingTexture != 0 || capturingList != 0; }

	int CreateList(const std::function<void()>& drawFunc)
	{
		const int listID = nextListID++;
		auto& list = lists[listID];
		list.clear();

		const int previousList = capturingList;
		const auto previousStack = matrixStack;
		const auto previousColor = color;
		const int previousTexture = boundTexture;
		const bool previousBlendEnabled = blendEnabled;
		const uint32_t previousBlendSrcColor = blendSrcColor;
		const uint32_t previousBlendDstColor = blendDstColor;
		const uint32_t previousBlendSrcAlpha = blendSrcAlpha;
		const uint32_t previousBlendDstAlpha = blendDstAlpha;
		capturingList = listID;
		drawFunc();
		capturingList = previousList;
		matrixStack = previousStack;
		color = previousColor;
		boundTexture = previousTexture;
		blendEnabled = previousBlendEnabled;
		blendSrcColor = previousBlendSrcColor;
		blendDstColor = previousBlendDstColor;
		blendSrcAlpha = previousBlendSrcAlpha;
		blendDstAlpha = previousBlendDstAlpha;

		return listID;
	}

	void DeleteList(int listID)
	{
		lists.erase(listID);
	}

	void CallList(int listID)
	{
		const auto it = lists.find(listID);
		if (it == lists.end())
			return;

		for (const ListCommand& command: it->second) {
			switch (command.type) {
				case ListCommand::Type::Text: {
					DrawText(command.text);
				} break;
				case ListCommand::Type::Rect: {
					const auto previousColor = color;
					std::copy(command.rect.color, command.rect.color + 4, color.begin());
					DrawRect(command.rect.x1, command.rect.y1, command.rect.x2, command.rect.y2);
					color = previousColor;
				} break;
				case ListCommand::Type::Triangle: {
					DrawTriangleColored(
						command.x1, command.y1, command.color,
						command.x2, command.y2, command.color2,
						command.x3, command.y3, command.color3);
				} break;
				case ListCommand::Type::Texture: {
					const auto previousColor = color;
					const int previousTexture = boundTexture;
					std::copy(command.color, command.color + 4, color.begin());
					const int texForDraw = command.textureID != 0 ? command.textureID : boundTexture;
					boundTexture = texForDraw;
					DrawBoundTextureRectUV(command.x1, command.y1, command.x2, command.y2, command.u1, command.v1, command.u2, command.v2);
					boundTexture = previousTexture;
					color = previousColor;
				} break;
				case ListCommand::Type::TextureTriangle: {
					const int previousTexture = boundTexture;
					const int texForDraw = command.textureID != 0 ? command.textureID : boundTexture;
					boundTexture = texForDraw;
					DrawBoundTexturedTriangle(
						command.x1, command.y1, command.u1, command.v1, command.color,
						command.x2, command.y2, command.u2, command.v2, command.color2,
						command.x3, command.y3, command.u3, command.v3, command.color3);
					boundTexture = previousTexture;
				} break;
				case ListCommand::Type::BindTexture: {
					boundTexture = command.textureID;
				} break;
				case ListCommand::Type::Color: {
					std::copy(command.color, command.color + 4, color.begin());
				} break;
				case ListCommand::Type::BlendState: {
					blendEnabled = command.boolValue;
					blendSrcColor = command.srcColor;
					blendDstColor = command.dstColor;
					blendSrcAlpha = command.srcAlpha;
					blendDstAlpha = command.dstAlpha;
				} break;
				case ListCommand::Type::PushMatrix: {
					PushMatrix();
				} break;
				case ListCommand::Type::PopMatrix: {
					PopMatrix();
				} break;
				case ListCommand::Type::LoadIdentity: {
					LoadIdentity();
				} break;
				case ListCommand::Type::Ortho: {
					Ortho(command.x1, command.x2, command.y1, command.y2);
				} break;
				case ListCommand::Type::Translate: {
					Translate(command.x1, command.y1);
				} break;
				case ListCommand::Type::Rotate: {
					Rotate(command.x1);
				} break;
				case ListCommand::Type::Scale: {
					Scale(command.x1, command.y1);
				} break;
			}
		}
	}

	void PushMatrix()
	{
		RecordStateCommand(ListCommand::Type::PushMatrix);
		matrixStack.push_back(CurrentMatrix());
	}

	void PopMatrix()
	{
		RecordStateCommand(ListCommand::Type::PopMatrix);
		if (matrixStack.size() > 1)
			matrixStack.pop_back();
	}

	void LoadIdentity()
	{
		RecordStateCommand(ListCommand::Type::LoadIdentity);
		CurrentMatrix() = Affine2D{};
	}

	void Ortho(float left, float right, float bottom, float top)
	{
		RecordMatrixCommand(ListCommand::Type::Ortho, left, bottom, right, top);
		const float width = right - left;
		const float height = top - bottom;
		if (width == 0.0f || height == 0.0f)
			return;

		CurrentMatrix() = Affine2D{
			.a = 2.0f / width,
			.b = 0.0f,
			.c = 0.0f,
			.d = 2.0f / height,
			.tx = -(right + left) / width,
			.ty = -(top + bottom) / height,
		};
	}

	void Translate(float x, float y)
	{
		RecordMatrixCommand(ListCommand::Type::Translate, x, y, 0.0f, 0.0f);
		CurrentMatrix().PostTranslate(x, y);
	}

	void Scale(float x, float y)
	{
		RecordMatrixCommand(ListCommand::Type::Scale, x, y, 0.0f, 0.0f);
		CurrentMatrix().PostScale(x, y);
	}

	void Rotate(float degrees)
	{
		RecordMatrixCommand(ListCommand::Type::Rotate, degrees, 0.0f, 0.0f, 0.0f);
		CurrentMatrix().PostRotate(degrees);
	}

	void SetColor(float r, float g, float b, float a)
	{
		color = {r, g, b, a};
		if (capturingList != 0) {
			ListCommand command;
			command.type = ListCommand::Type::Color;
			std::copy(color.begin(), color.end(), command.color);
			lists[capturingList].push_back(command);
		}
	}

	void SetScissor(bool enabled, int x, int y, int width, int height)
	{
		scissorEnabled = enabled;
		scissorX = x;
		scissorY = y;
		scissorW = std::max(0, width);
		scissorH = std::max(0, height);
	}

	void SetBlending(bool enabled)
	{
		blendEnabled = enabled;
		RecordBlendCommand();
	}

	void SetBlendFunc(uint32_t src, uint32_t dst)
	{
		SetBlendFuncSeparate(src, dst, src, dst);
	}

	void SetBlendFuncSeparate(uint32_t srcColor, uint32_t dstColor, uint32_t srcAlpha, uint32_t dstAlpha)
	{
		blendEnabled = true;
		blendSrcColor = srcColor;
		blendDstColor = dstColor;
		blendSrcAlpha = srcAlpha;
		blendDstAlpha = dstAlpha;
		RecordBlendCommand();
	}

	void DrawText(LuaUITextDraw draw)
	{
		draw.text = StripColorCodes(draw.text);
		if (draw.text.empty())
			return;

		if (capturingList != 0) {
			ListCommand command;
			command.type = ListCommand::Type::Text;
			command.text = draw;
			lists[capturingList].push_back(std::move(command));
			return;
		}

		if (capturingTexture != 0) {
			CaptureText(draw);
			return;
		}

		DrawTextScreen(draw, float(globalRendering->viewSizeX));
	}

	void DrawRect(float x1, float y1, float x2, float y2)
	{
		LuaUIRectDraw rect;
		rect.x1 = x1;
		rect.y1 = y1;
		rect.x2 = x2;
		rect.y2 = y2;
		std::copy(color.begin(), color.end(), rect.color);

		if (capturingList != 0) {
			ListCommand command;
			command.type = ListCommand::Type::Rect;
			command.rect = rect;
			lists[capturingList].push_back(std::move(command));
			return;
		}

		if (capturingTexture != 0) {
			CaptureRect(rect);
			return;
		}

		const auto matrix = CurrentMatrix();
		DrawRectScreenQuad(
			rect,
			matrix.Transform(x1, y1),
			matrix.Transform(x2, y1),
			matrix.Transform(x1, y2),
			matrix.Transform(x2, y2)
		);
	}

	void DrawTriangle(float x1, float y1, float x2, float y2, float x3, float y3)
	{
		const float c[4] = {color[0], color[1], color[2], color[3]};
		DrawTriangleColored(x1, y1, c, x2, y2, c, x3, y3, c);
	}

	void DrawTriangleColored(
		float x1, float y1, const float color1[4],
		float x2, float y2, const float color2[4],
		float x3, float y3, const float color3[4])
	{
		if (capturingList != 0) {
			ListCommand command;
			command.type = ListCommand::Type::Triangle;
			command.x1 = x1; command.y1 = y1;
			command.x2 = x2; command.y2 = y2;
			command.x3 = x3; command.y3 = y3;
			std::copy(color1, color1 + 4, command.color);
			std::copy(color2, color2 + 4, command.color2);
			std::copy(color3, color3 + 4, command.color3);
			lists[capturingList].push_back(std::move(command));
			return;
		}

		if (capturingTexture != 0) {
			CaptureTriangle(x1, y1, color1, x2, y2, color2, x3, y3, color3);
			return;
		}

		// Direct screen-render path: replayed display lists (wind dial, com
		// counter, etc.) emit triangles outside any capture; without this the
		// rect pipeline never sees them and the icons render empty.
		const auto matrix = CurrentMatrix();
		DrawTriangleScreen(
			matrix.Transform(x1, y1), color1,
			matrix.Transform(x2, y2), color2,
			matrix.Transform(x3, y3), color3);
	}

	void DrawBoundTexturedTriangle(
		float x1, float y1, float u1, float v1, const float color1[4],
		float x2, float y2, float u2, float v2, const float color2[4],
		float x3, float y3, float u3, float v3, const float color3[4])
	{
		if (capturingList != 0) {
			ListCommand command;
			command.type = ListCommand::Type::TextureTriangle;
			command.textureID = boundTexture;
			command.x1 = x1; command.y1 = y1; command.u1 = u1; command.v1 = v1;
			command.x2 = x2; command.y2 = y2; command.u2 = u2; command.v2 = v2;
			command.x3 = x3; command.y3 = y3; command.u3 = u3; command.v3 = v3;
			std::copy(color1, color1 + 4, command.color);
			std::copy(color2, color2 + 4, command.color2);
			std::copy(color3, color3 + 4, command.color3);
			lists[capturingList].push_back(std::move(command));
			return;
		}

		const auto it = textures.find(boundTexture);
		if (it == textures.end())
			return;

		const auto& texture = it->second;
		if (capturingTexture != 0) {
			CaptureTexturedTriangle(texture,
				x1, y1, u1, v1, color1,
				x2, y2, u2, v2, color2,
				x3, y3, u3, v3, color3);
			return;
		}

		const auto matrix = CurrentMatrix();
		DrawTexturedTriangleScreen(
			texture,
			matrix.Transform(x1, y1), u1, v1, color1,
			matrix.Transform(x2, y2), u2, v2, color2,
			matrix.Transform(x3, y3), u3, v3, color3);
	}

	void DrawBoundTextureRect(float x1, float y1, float x2, float y2)
	{
		DrawBoundTextureRectUV(x1, y1, x2, y2, 0.0f, 1.0f, 1.0f, 0.0f);
	}

	void DrawBoundTextureRectUV(float x1, float y1, float x2, float y2, float u1, float v1, float u2, float v2)
	{
		if (capturingList != 0) {
			ListCommand command;
			command.type = ListCommand::Type::Texture;
			command.textureID = boundTexture;
			command.x1 = x1;
			command.y1 = y1;
			command.x2 = x2;
			command.y2 = y2;
			command.u1 = u1;
			command.v1 = v1;
			command.u2 = u2;
			command.v2 = v2;
			std::copy(color.begin(), color.end(), command.color);
			lists[capturingList].push_back(std::move(command));
			return;
		}

		if (std::getenv("SPRING_METAL_LUAUI_TRACE") != nullptr && std::abs(x2 - x1) > 1000.0f) {
			LOG_L(L_INFO, "[MetalLuaUI] texrect lookup bound=%d rect=(%.1f,%.1f)-(%.1f,%.1f)", boundTexture, x1, y1, x2, y2);
		}
		const auto it = textures.find(boundTexture);
		if (it == textures.end()) {
			if (std::getenv("SPRING_METAL_LUAUI_TRACE") != nullptr && std::abs(x2 - x1) > 1000.0f)
				LOG_L(L_INFO, "[MetalLuaUI] texrect missing bound texture %d", boundTexture);
			return;
		}

		const auto& texture = it->second;
		if (std::getenv("SPRING_METAL_LUAUI_TRACE") != nullptr && std::abs(x2 - x1) > 1000.0f) {
			LOG_L(L_INFO, "[MetalLuaUI] texrect found tex=%d size=%dx%d valid=%d dirty=%d", boundTexture, texture.desc.width, texture.desc.height, texture.texture && texture.texture->IsValid() ? 1 : 0, texture.dirty ? 1 : 0);
		}
		if (capturingTexture != 0) {
			CaptureTextureRect(texture, x1, y1, x2, y2, u1, v1, u2, v2);
			return;
		}

		const auto matrix = CurrentMatrix();
		const auto p00 = matrix.Transform(x1, y1);
		const auto p10 = matrix.Transform(x2, y1);
		const auto p01 = matrix.Transform(x1, y2);
		const auto p11 = matrix.Transform(x2, y2);
		DrawTextureScreenQuad(texture, p00, p10, p01, p11, u1, v1, u2, v2);

		for (const CapturedText& text: texture.texts) {
			const auto lerp = [](float a, float b, float t) {
				return a + (b - a) * t;
			};
			constexpr float eps = 1.0e-6f;
			const float texU = text.localX / float(texture.desc.width);
			const float texV = text.localY / float(texture.desc.height);
			const float quadU = (std::abs(u2 - u1) > eps) ? (texU - u1) / (u2 - u1) : texU;
			const float quadV = (std::abs(v2 - v1) > eps) ? (texV - v1) / (v2 - v1) : texV;
			const float leftX = lerp(p00.first, p01.first, quadV);
			const float rightX = lerp(p10.first, p11.first, quadV);
			const float topY = lerp(p01.second, p11.second, quadU);
			const float bottomY = lerp(p00.second, p10.second, quadU);

			LuaUITextDraw draw = text.draw;
			draw.x = lerp(leftX, rightX, quadU);
			draw.y = lerp(bottomY, topY, quadV);
			draw.size = text.localSize * (std::abs(topY - bottomY) / float(texture.desc.height));
			DrawTextScreen(draw, std::max(leftX, rightX));
		}
	}

private:
	void RecordTextureBind(int textureID)
	{
		if (capturingList == 0)
			return;

		ListCommand command;
		command.type = ListCommand::Type::BindTexture;
		command.textureID = textureID;
		lists[capturingList].push_back(command);
	}

	void RecordBlendCommand()
	{
		if (capturingList == 0)
			return;

		ListCommand command;
		command.type = ListCommand::Type::BlendState;
		command.boolValue = blendEnabled;
		command.srcColor = blendSrcColor;
		command.dstColor = blendDstColor;
		command.srcAlpha = blendSrcAlpha;
		command.dstAlpha = blendDstAlpha;
		lists[capturingList].push_back(command);
	}

	void RecordStateCommand(ListCommand::Type type)
	{
		if (capturingList == 0)
			return;

		ListCommand command;
		command.type = type;
		lists[capturingList].push_back(command);
	}

	void RecordMatrixCommand(ListCommand::Type type, float x1, float y1, float x2, float y2)
	{
		if (capturingList == 0)
			return;

		ListCommand command;
		command.type = type;
		command.x1 = x1;
		command.y1 = y1;
		command.x2 = x2;
		command.y2 = y2;
		lists[capturingList].push_back(command);
	}

	Affine2D& CurrentMatrix()
	{
		if (matrixStack.empty())
			matrixStack.push_back(Affine2D{});
		return matrixStack.back();
	}

	Affine2D CurrentMatrix() const
	{
		return matrixStack.empty() ? Affine2D{} : matrixStack.back();
	}

	std::pair<float, float> ClipToTextureLocal(float clipX, float clipY, const LuaUITextureDesc& desc) const
	{
		return {
			(clipX + 1.0f) * 0.5f * desc.width,
			(clipY + 1.0f) * 0.5f * desc.height,
		};
	}

	void CaptureText(const LuaUITextDraw& draw)
	{
		auto it = textures.find(capturingTexture);
		if (it == textures.end())
			return;

		const auto matrix = CurrentMatrix();
		const auto [clipX, clipY] = matrix.Transform(draw.x, draw.y);
		const auto [clipX2, clipY2] = matrix.Transform(draw.x, draw.y + draw.size);
		const auto [localX, localY] = ClipToTextureLocal(clipX, clipY, it->second.desc);
		const auto [localX2, localY2] = ClipToTextureLocal(clipX2, clipY2, it->second.desc);
		if (scissorEnabled && !LocalRectIntersectsCaptureClip(localX, localY, localX2, localY2))
			return;

		CapturedText captured;
		captured.draw = draw;
		captured.localX = localX;
		captured.localY = localY;
		captured.localSize = std::max(1.0f, std::abs(localY2 - localY));
		it->second.texts.push_back(std::move(captured));
	}

	void CaptureRect(const LuaUIRectDraw& rect)
	{
		auto it = textures.find(capturingTexture);
		if (it == textures.end())
			return;

		const auto matrix = CurrentMatrix();
		const auto [clipX1, clipY1] = matrix.Transform(rect.x1, rect.y1);
		const auto [clipX2, clipY2] = matrix.Transform(rect.x2, rect.y2);
		const auto [localX1, localY1] = ClipToTextureLocal(clipX1, clipY1, it->second.desc);
		const auto [localX2, localY2] = ClipToTextureLocal(clipX2, clipY2, it->second.desc);
		if (scissorEnabled && !LocalRectIntersectsCaptureClip(localX1, localY1, localX2, localY2))
			return;

		RasterizeRect(it->second, rect, localX1, localY1, localX2, localY2);
	}

	void CaptureTextureRect(const TextureCommandBuffer& source, float x1, float y1, float x2, float y2,
	                        float u1, float v1, float u2, float v2)
	{
		if (source.pixels.empty())
			return;

		auto it = textures.find(capturingTexture);
		if (it == textures.end())
			return;

		const auto matrix = CurrentMatrix();
		const auto [clipX1, clipY1] = matrix.Transform(x1, y1);
		const auto [clipX2, clipY2] = matrix.Transform(x2, y2);
		const auto [localX1, localY1] = ClipToTextureLocal(clipX1, clipY1, it->second.desc);
		const auto [localX2, localY2] = ClipToTextureLocal(clipX2, clipY2, it->second.desc);
		if (scissorEnabled && !LocalRectIntersectsCaptureClip(localX1, localY1, localX2, localY2))
			return;

		RasterizeTextureRect(it->second, source, localX1, localY1, localX2, localY2, u1, v1, u2, v2);
	}

	void CaptureTexturedTriangle(
		const TextureCommandBuffer& source,
		float x1, float y1, float u1, float v1, const float color1[4],
		float x2, float y2, float u2, float v2, const float color2[4],
		float x3, float y3, float u3, float v3, const float color3[4])
	{
		if (source.pixels.empty())
			return;

		auto it = textures.find(capturingTexture);
		if (it == textures.end())
			return;

		const auto matrix = CurrentMatrix();
		const auto [clipX1, clipY1] = matrix.Transform(x1, y1);
		const auto [clipX2, clipY2] = matrix.Transform(x2, y2);
		const auto [clipX3, clipY3] = matrix.Transform(x3, y3);
		const auto [localX1, localY1] = ClipToTextureLocal(clipX1, clipY1, it->second.desc);
		const auto [localX2, localY2] = ClipToTextureLocal(clipX2, clipY2, it->second.desc);
		const auto [localX3, localY3] = ClipToTextureLocal(clipX3, clipY3, it->second.desc);

		RasterizeTexturedTriangle(
			it->second, source,
			localX1, localY1, u1, v1, color1,
			localX2, localY2, u2, v2, color2,
			localX3, localY3, u3, v3, color3);
	}

	void CaptureTriangle(
		float x1, float y1, const float color1[4],
		float x2, float y2, const float color2[4],
		float x3, float y3, const float color3[4])
	{
		auto it = textures.find(capturingTexture);
		if (it == textures.end())
			return;

		const auto matrix = CurrentMatrix();
		const auto [clipX1, clipY1] = matrix.Transform(x1, y1);
		const auto [clipX2, clipY2] = matrix.Transform(x2, y2);
		const auto [clipX3, clipY3] = matrix.Transform(x3, y3);
		const auto [localX1, localY1] = ClipToTextureLocal(clipX1, clipY1, it->second.desc);
		const auto [localX2, localY2] = ClipToTextureLocal(clipX2, clipY2, it->second.desc);
		const auto [localX3, localY3] = ClipToTextureLocal(clipX3, clipY3, it->second.desc);

		RasterizeTriangle(
			it->second,
			localX1, localY1, color1,
			localX2, localY2, color2,
			localX3, localY3, color3);
	}

	bool LocalRectIntersectsCaptureClip(float x1, float y1, float x2, float y2) const
	{
		const float minX = std::min(x1, x2);
		const float maxX = std::max(x1, x2);
		const float minY = std::min(y1, y2);
		const float maxY = std::max(y1, y2);
		return maxX >= scissorX && minX <= scissorX + scissorW &&
		       maxY >= scissorY && minY <= scissorY + scissorH;
	}

	static uint32_t PackColor(const float* color)
	{
		const auto clampByte = [](float v) {
			return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
		};

		const uint32_t r = clampByte(color[0]);
		const uint32_t g = clampByte(color[1]);
		const uint32_t b = clampByte(color[2]);
		const uint32_t a = clampByte(color[3]);
		return r | (g << 8) | (b << 16) | (a << 24);
	}

	static uint32_t TintPixel(uint32_t pixel, const std::array<float, 4>& tint, bool blendEnabled)
	{
		const auto scaleByte = [](uint32_t value, float scale) {
			return uint32_t(std::clamp(float(value) * scale, 0.0f, 255.0f) + 0.5f);
		};

		const uint32_t r = scaleByte( pixel        & 0xFFu, tint[0]);
		const uint32_t g = scaleByte((pixel >>  8) & 0xFFu, tint[1]);
		const uint32_t b = scaleByte((pixel >> 16) & 0xFFu, tint[2]);
		const uint32_t a = blendEnabled ? scaleByte((pixel >> 24) & 0xFFu, tint[3]) : 0xFFu;
		return r | (g << 8) | (b << 16) | (a << 24);
	}

	static uint32_t TintPixel(uint32_t pixel, const float tint[4], bool blendEnabled)
	{
		const auto scaleByte = [](uint32_t value, float scale) {
			return uint32_t(std::clamp(float(value) * scale, 0.0f, 255.0f) + 0.5f);
		};

		const uint32_t r = scaleByte( pixel        & 0xFFu, tint[0]);
		const uint32_t g = scaleByte((pixel >>  8) & 0xFFu, tint[1]);
		const uint32_t b = scaleByte((pixel >> 16) & 0xFFu, tint[2]);
		const uint32_t a = blendEnabled ? scaleByte((pixel >> 24) & 0xFFu, tint[3]) : 0xFFu;
		return r | (g << 8) | (b << 16) | (a << 24);
	}

	static uint32_t BlendPixel(uint32_t dst, uint32_t src,
	                           bool enabled,
	                           uint32_t srcColorFactor, uint32_t dstColorFactor,
	                           uint32_t srcAlphaFactor, uint32_t dstAlphaFactor)
	{
		if (!enabled)
			return src;

		const uint32_t srcA = (src >> 24) & 0xFFu;
		const uint32_t dstA = (dst >> 24) & 0xFFu;
		const auto factor = [&](uint32_t blendFactor) {
			switch (blendFactor) {
				case GL_ZERO: return 0u;
				case GL_ONE: return 255u;
				case GL_SRC_ALPHA: return srcA;
				case GL_ONE_MINUS_SRC_ALPHA: return 255u - srcA;
				case GL_DST_ALPHA: return dstA;
				case GL_ONE_MINUS_DST_ALPHA: return 255u - dstA;
				default: return 255u;
			}
		};

		const uint32_t srcColorMul = factor(srcColorFactor);
		const uint32_t dstColorMul = factor(dstColorFactor);
		const uint32_t srcAlphaMul = factor(srcAlphaFactor);
		const uint32_t dstAlphaMul = factor(dstAlphaFactor);
		const auto blendByte = [](uint32_t srcByte, uint32_t dstByte, uint32_t srcMul, uint32_t dstMul) {
			return std::min(255u, (srcByte * srcMul + dstByte * dstMul) / 255u);
		};

		const uint32_t r = blendByte( src        & 0xFFu,  dst        & 0xFFu, srcColorMul, dstColorMul);
		const uint32_t g = blendByte((src >>  8) & 0xFFu, (dst >>  8) & 0xFFu, srcColorMul, dstColorMul);
		const uint32_t b = blendByte((src >> 16) & 0xFFu, (dst >> 16) & 0xFFu, srcColorMul, dstColorMul);
		const uint32_t a = blendByte(srcA, dstA, srcAlphaMul, dstAlphaMul);
		return r | (g << 8) | (b << 16) | (a << 24);
	}

	static int RepeatTexel(float coord, int size)
	{
		if (size <= 1)
			return 0;

		const float wrapped = coord - std::floor(coord);
		return std::clamp(int(wrapped * float(size - 1) + 0.5f), 0, size - 1);
	}

	void RasterizeRect(TextureCommandBuffer& texture, const LuaUIRectDraw& rect,
	                  float localX1, float localY1, float localX2, float localY2)
	{
		if (texture.pixels.empty())
			return;

		int x0 = std::clamp(int(std::floor(std::min(localX1, localX2))), 0, texture.desc.width);
		int x1 = std::clamp(int(std::ceil (std::max(localX1, localX2))), 0, texture.desc.width);
		int y0 = std::clamp(int(std::floor(std::min(localY1, localY2))), 0, texture.desc.height);
		int y1 = std::clamp(int(std::ceil (std::max(localY1, localY2))), 0, texture.desc.height);
		ApplyCaptureClip(x0, y0, x1, y1);
		if (x0 >= x1 || y0 >= y1)
			return;

		const uint32_t packed = PackColor(rect.color);
		for (int y = y0; y < y1; ++y) {
			uint32_t* row = texture.pixels.data() + y * texture.desc.width;
			if (!blendEnabled) {
				std::fill(row + x0, row + x1, packed);
			} else {
				for (int x = x0; x < x1; ++x)
					row[x] = BlendPixel(row[x], packed, blendEnabled, blendSrcColor, blendDstColor, blendSrcAlpha, blendDstAlpha);
			}
		}
		texture.dirty = true;
	}

	void RasterizeTriangle(TextureCommandBuffer& texture,
	                       float x1, float y1, const float color1[4],
	                       float x2, float y2, const float color2[4],
	                       float x3, float y3, const float color3[4])
	{
		if (texture.pixels.empty())
			return;

		const float minX = std::min({x1, x2, x3});
		const float maxX = std::max({x1, x2, x3});
		const float minY = std::min({y1, y2, y3});
		const float maxY = std::max({y1, y2, y3});
		int x0 = std::clamp(int(std::floor(minX)), 0, texture.desc.width);
		int xEnd = std::clamp(int(std::ceil(maxX)), 0, texture.desc.width);
		int y0 = std::clamp(int(std::floor(minY)), 0, texture.desc.height);
		int yEnd = std::clamp(int(std::ceil(maxY)), 0, texture.desc.height);
		const int preClipX0 = x0, preClipY0 = y0, preClipXEnd = xEnd, preClipYEnd = yEnd;
		ApplyCaptureClip(x0, y0, xEnd, yEnd);
		if (std::getenv("SPRING_METAL_LUAUI_RASTRACE") != nullptr && texture.desc.width > 1000 && texture.desc.height < 100) {
			LOG_L(L_INFO, "[MetalLuaUI rasterTri] tex=%dx%d tri=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) col1=(%.2f,%.2f,%.2f,%.2f) preClip=(%d,%d-%d,%d) postClip=(%d,%d-%d,%d) blend=%d srcC=%u dstC=%u srcA=%u dstA=%u",
				texture.desc.width, texture.desc.height,
				x1, y1, x2, y2, x3, y3,
				color1[0], color1[1], color1[2], color1[3],
				preClipX0, preClipY0, preClipXEnd, preClipYEnd,
				x0, y0, xEnd, yEnd,
				blendEnabled ? 1 : 0,
				blendSrcColor, blendDstColor, blendSrcAlpha, blendDstAlpha);
		}
		if (x0 >= xEnd || y0 >= yEnd)
			return;

		const auto edge = [](float ax, float ay, float bx, float by, float px, float py) {
			return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
		};
		const float area = edge(x1, y1, x2, y2, x3, y3);
		if (area == 0.0f)
			return;
		const float invArea = 1.0f / area;

		// Bake any uniform-color shortcut: skip per-pixel interpolation when
		// all three vertex colors match (rasterizer hot path for solid fills).
		const bool uniformColor =
			color1[0] == color2[0] && color1[0] == color3[0] &&
			color1[1] == color2[1] && color1[1] == color3[1] &&
			color1[2] == color2[2] && color1[2] == color3[2] &&
			color1[3] == color2[3] && color1[3] == color3[3];
		const uint32_t uniformPacked = uniformColor ? PackColor(color1) : 0u;

		int writeCount = 0;
		uint32_t lastWrittenAfter = 0;
		for (int y = y0; y < yEnd; ++y) {
			uint32_t* row = texture.pixels.data() + y * texture.desc.width;
			for (int x = x0; x < xEnd; ++x) {
				const float px = float(x) + 0.5f;
				const float py = float(y) + 0.5f;
				const float e23 = edge(x2, y2, x3, y3, px, py);
				const float e31 = edge(x3, y3, x1, y1, px, py);
				const float e12 = edge(x1, y1, x2, y2, px, py);
				const bool inside =
					(area > 0.0f && e23 >= 0.0f && e31 >= 0.0f && e12 >= 0.0f) ||
					(area < 0.0f && e23 <= 0.0f && e31 <= 0.0f && e12 <= 0.0f);
				if (!inside)
					continue;

				uint32_t src;
				if (uniformColor) {
					src = uniformPacked;
				} else {
					// Barycentric weights: w1 weights vertex 1, derived from
					// the edge opposite to it (e23), etc.
					const float w1 = e23 * invArea;
					const float w2 = e31 * invArea;
					const float w3 = e12 * invArea;
					const float interp[4] = {
						color1[0] * w1 + color2[0] * w2 + color3[0] * w3,
						color1[1] * w1 + color2[1] * w2 + color3[1] * w3,
						color1[2] * w1 + color2[2] * w2 + color3[2] * w3,
						color1[3] * w1 + color2[3] * w2 + color3[3] * w3,
					};
					src = PackColor(interp);
				}
				row[x] = BlendPixel(row[x], src, blendEnabled, blendSrcColor, blendDstColor, blendSrcAlpha, blendDstAlpha);
				lastWrittenAfter = row[x];
				++writeCount;
			}
		}
		if (std::getenv("SPRING_METAL_LUAUI_RASTRACE") != nullptr && texture.desc.width > 1000 && texture.desc.height < 100) {
			LOG_L(L_INFO, "[MetalLuaUI rasterTri] capture=%d wrote=%d lastAfter=#%08x area=%.1f",
				capturingTexture, writeCount, lastWrittenAfter, area);
		}
		texture.dirty = true;
	}

	void RasterizeTexturedTriangle(TextureCommandBuffer& target, const TextureCommandBuffer& source,
	                               float x1, float y1, float u1, float v1, const float color1[4],
	                               float x2, float y2, float u2, float v2, const float color2[4],
	                               float x3, float y3, float u3, float v3, const float color3[4])
	{
		if (target.pixels.empty() || source.pixels.empty())
			return;

		const float minX = std::min({x1, x2, x3});
		const float maxX = std::max({x1, x2, x3});
		const float minY = std::min({y1, y2, y3});
		const float maxY = std::max({y1, y2, y3});
		int x0 = std::clamp(int(std::floor(minX)), 0, target.desc.width);
		int xEnd = std::clamp(int(std::ceil(maxX)), 0, target.desc.width);
		int y0 = std::clamp(int(std::floor(minY)), 0, target.desc.height);
		int yEnd = std::clamp(int(std::ceil(maxY)), 0, target.desc.height);
		ApplyCaptureClip(x0, y0, xEnd, yEnd);
		if (x0 >= xEnd || y0 >= yEnd)
			return;

		const auto edge = [](float ax, float ay, float bx, float by, float px, float py) {
			return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
		};
		const float area = edge(x1, y1, x2, y2, x3, y3);
		if (area == 0.0f)
			return;
		const float invArea = 1.0f / area;

		for (int y = y0; y < yEnd; ++y) {
			uint32_t* dstRow = target.pixels.data() + y * target.desc.width;
			for (int x = x0; x < xEnd; ++x) {
				const float px = float(x) + 0.5f;
				const float py = float(y) + 0.5f;
				const float e23 = edge(x2, y2, x3, y3, px, py);
				const float e31 = edge(x3, y3, x1, y1, px, py);
				const float e12 = edge(x1, y1, x2, y2, px, py);
				const bool inside =
					(area > 0.0f && e23 >= 0.0f && e31 >= 0.0f && e12 >= 0.0f) ||
					(area < 0.0f && e23 <= 0.0f && e31 <= 0.0f && e12 <= 0.0f);
				if (!inside)
					continue;

				const float w1 = e23 * invArea;
				const float w2 = e31 * invArea;
				const float w3 = e12 * invArea;
				const float u = u1 * w1 + u2 * w2 + u3 * w3;
				const float v = v1 * w1 + v2 * w2 + v3 * w3;
				const int srcX = RepeatTexel(u, source.desc.width);
				const int srcY = RepeatTexel(v, source.desc.height);
				const uint32_t* srcRow = source.pixels.data() + srcY * source.desc.width;
				const float interpColor[4] = {
					color1[0] * w1 + color2[0] * w2 + color3[0] * w3,
					color1[1] * w1 + color2[1] * w2 + color3[1] * w3,
					color1[2] * w1 + color2[2] * w2 + color3[2] * w3,
					color1[3] * w1 + color2[3] * w2 + color3[3] * w3,
				};
				const uint32_t src = TintPixel(srcRow[srcX], interpColor, blendEnabled);
				dstRow[x] = BlendPixel(dstRow[x], src, blendEnabled, blendSrcColor, blendDstColor, blendSrcAlpha, blendDstAlpha);
			}
		}
		target.dirty = true;
	}

	void RasterizeTextureRect(TextureCommandBuffer& target, const TextureCommandBuffer& source,
	                          float localX1, float localY1, float localX2, float localY2,
	                          float u1, float v1, float u2, float v2)
	{
		if (target.pixels.empty() || source.pixels.empty())
			return;

		const float minX = std::min(localX1, localX2);
		const float maxX = std::max(localX1, localX2);
		const float minY = std::min(localY1, localY2);
		const float maxY = std::max(localY1, localY2);
		int x0 = std::clamp(int(std::floor(minX)), 0, target.desc.width);
		int x1 = std::clamp(int(std::ceil (maxX)), 0, target.desc.width);
		int y0 = std::clamp(int(std::floor(minY)), 0, target.desc.height);
		int y1 = std::clamp(int(std::ceil (maxY)), 0, target.desc.height);
		ApplyCaptureClip(x0, y0, x1, y1);
		if (x0 >= x1 || y0 >= y1)
			return;

		const float invWidth = (localX2 != localX1) ? 1.0f / (localX2 - localX1) : 0.0f;
		const float invHeight = (localY2 != localY1) ? 1.0f / (localY2 - localY1) : 0.0f;
		for (int y = y0; y < y1; ++y) {
			uint32_t* dstRow = target.pixels.data() + y * target.desc.width;
			const float v = v1 + (v2 - v1) * std::clamp(((float(y) + 0.5f) - localY1) * invHeight, 0.0f, 1.0f);
			const int srcY = RepeatTexel(v, source.desc.height);
			const uint32_t* srcRow = source.pixels.data() + srcY * source.desc.width;
			for (int x = x0; x < x1; ++x) {
				const float u = u1 + (u2 - u1) * std::clamp(((float(x) + 0.5f) - localX1) * invWidth, 0.0f, 1.0f);
				const int srcX = RepeatTexel(u, source.desc.width);
				const uint32_t src = TintPixel(srcRow[srcX], color, blendEnabled);
				dstRow[x] = BlendPixel(dstRow[x], src, blendEnabled, blendSrcColor, blendDstColor, blendSrcAlpha, blendDstAlpha);
			}
		}
		target.dirty = true;
	}

	void ApplyCaptureClip(int& x0, int& y0, int& x1, int& y1) const
	{
		if (!scissorEnabled)
			return;

		x0 = std::max(x0, scissorX);
		y0 = std::max(y0, scissorY);
		x1 = std::min(x1, scissorX + scissorW);
		y1 = std::min(y1, scissorY + scissorH);
	}

	void DrawTextScreen(const LuaUITextDraw& draw, float maxX)
	{
		if (!valid || draw.text.empty() || globalRendering == nullptr)
			return;
		if (!ApplyScissor())
			return;

		float size = draw.size;
		if (draw.options & FONT_SCALE)
			size *= draw.fontSize;

		const float viewSizeX = std::max(1.0f, float(globalRendering->viewSizeX));
		const float viewSizeY = std::max(1.0f, float(globalRendering->viewSizeY));
		const bool normalized = (draw.options & FONT_NORM) != 0;
		const float normX = normalized ? draw.x : draw.x / viewSizeX;
		const float normY = normalized ? draw.y : draw.y / viewSizeY;
		const float glyphH = std::max(2.0f * size / viewSizeY, 0.001f);

		float ndcX = normX * 2.0f - 1.0f;
		float ndcTop = normY * 2.0f - 1.0f + glyphH;

		// Pixel width of the text as the LuaUI widget computed positions for it.
		// Prefer real CglFont metrics so 'c'/'r' alignment and button-width
		// layouts (which compute via GetTextWidth/Print) line up with what
		// our overlay actually rasterizes.
		float textPx = 0.0f;
		// draw.text was already color-stripped by DrawText() before reaching us.
		if (font != nullptr)
			textPx = font->GetTextWidth(draw.text) * size;
		if (textPx <= 0.0f) {
			// Fallback advance ratio close to a real proportional font.
			textPx = size * 0.5f * float(draw.text.size());
		}
		const float textNDC = (textPx / viewSizeX) * 2.0f;

		if (draw.options & FONT_CENTER)
			ndcX -= textNDC * 0.5f;
		else if (draw.options & FONT_RIGHT)
			ndcX -= textNDC;

		if (draw.options & FONT_TOP)
			ndcTop = normY * 2.0f - 1.0f;
		else if (draw.options & FONT_VCENTER)
			ndcTop = normY * 2.0f - 1.0f + glyphH * 0.5f;

		// Tell the overlay's auto-shrink to fit each line into the real font
		// width when that is tighter than `maxX`. Without this, our overlay's
		// fat-glyph advance bleeds neighbouring labels into each other.
		const float ndcMaxX = std::min(
			(maxX / viewSizeX) * 2.0f - 1.0f,
			ndcX + textNDC
		);
		textOverlay->DrawLine(ndcX, ndcTop, glyphH, draw.text, ndcMaxX);
	}

	void DrawRectScreen(const LuaUIRectDraw& rect)
	{
		DrawRectScreenQuad(
			rect,
			{rect.x1, rect.y1},
			{rect.x2, rect.y1},
			{rect.x1, rect.y2},
			{rect.x2, rect.y2}
		);
	}

	void DrawRectScreenQuad(const LuaUIRectDraw& rect,
	                        const std::pair<float, float>& p00,
	                        const std::pair<float, float>& p10,
	                        const std::pair<float, float>& p01,
	                        const std::pair<float, float>& p11)
	{
		if (!valid || globalRendering == nullptr)
			return;
		if (!ApplyScissor())
			return;

		const float viewSizeX = std::max(1.0f, float(globalRendering->viewSizeX));
		const float viewSizeY = std::max(1.0f, float(globalRendering->viewSizeY));
		const auto ndc = [&](float x, float y) {
			return std::pair<float, float>{x / viewSizeX * 2.0f - 1.0f, y / viewSizeY * 2.0f - 1.0f};
		};

		const auto [x00, y00] = ndc(p00.first, p00.second);
		const auto [x10, y10] = ndc(p10.first, p10.second);
		const auto [x01, y01] = ndc(p01.first, p01.second);
		const auto [x11, y11] = ndc(p11.first, p11.second);
		const float alpha = blendEnabled ? rect.color[3] : 1.0f;
		RectVertex verts[6] = {
			{{x00, y00}, {rect.color[0], rect.color[1], rect.color[2], alpha}},
			{{x10, y10}, {rect.color[0], rect.color[1], rect.color[2], alpha}},
			{{x01, y01}, {rect.color[0], rect.color[1], rect.color[2], alpha}},
			{{x01, y01}, {rect.color[0], rect.color[1], rect.color[2], alpha}},
			{{x10, y10}, {rect.color[0], rect.color[1], rect.color[2], alpha}},
			{{x11, y11}, {rect.color[0], rect.color[1], rect.color[2], alpha}},
		};

		rectVertexBuffer->UpdateData(verts, sizeof(verts), 0);
		rectPipeline->Enable();
		rectPipeline->BindVertexBuffer(0, *rectVertexBuffer);
		rectPipeline->Draw(PrimitiveTopology::Triangles, 0, 6);
		rectPipeline->Disable();
	}

	void DrawTriangleScreen(const std::pair<float, float>& p1, const float color1[4],
	                        const std::pair<float, float>& p2, const float color2[4],
	                        const std::pair<float, float>& p3, const float color3[4])
	{
		if (!valid || globalRendering == nullptr)
			return;
		if (!ApplyScissor())
			return;

		const float viewSizeX = std::max(1.0f, float(globalRendering->viewSizeX));
		const float viewSizeY = std::max(1.0f, float(globalRendering->viewSizeY));
		const auto ndc = [&](float x, float y) {
			return std::pair<float, float>{x / viewSizeX * 2.0f - 1.0f, y / viewSizeY * 2.0f - 1.0f};
		};

		const auto [x1, y1] = ndc(p1.first, p1.second);
		const auto [x2, y2] = ndc(p2.first, p2.second);
		const auto [x3, y3] = ndc(p3.first, p3.second);
		const auto pickAlpha = [&](const float c[4]) {
			return blendEnabled ? c[3] : 1.0f;
		};
		RectVertex verts[3] = {
			{{x1, y1}, {color1[0], color1[1], color1[2], pickAlpha(color1)}},
			{{x2, y2}, {color2[0], color2[1], color2[2], pickAlpha(color2)}},
			{{x3, y3}, {color3[0], color3[1], color3[2], pickAlpha(color3)}},
		};

		rectVertexBuffer->UpdateData(verts, sizeof(verts), 0);
		rectPipeline->Enable();
		rectPipeline->BindVertexBuffer(0, *rectVertexBuffer);
		rectPipeline->Draw(PrimitiveTopology::Triangles, 0, 3);
		rectPipeline->Disable();
	}

	void DrawTexturedTriangleScreen(const TextureCommandBuffer& texture,
	                                const std::pair<float, float>& p1, float u1, float v1, const float color1[4],
	                                const std::pair<float, float>& p2, float u2, float v2, const float color2[4],
	                                const std::pair<float, float>& p3, float u3, float v3, const float color3[4])
	{
		if (!valid || globalRendering == nullptr || texture.texture == nullptr || !texture.texture->IsValid())
			return;
		if (!ApplyScissor())
			return;

		if (texture.dirty) {
			texture.texture->UploadImage(texture.pixels.data());
			texture.dirty = false;
		}

		const float viewSizeX = std::max(1.0f, float(globalRendering->viewSizeX));
		const float viewSizeY = std::max(1.0f, float(globalRendering->viewSizeY));
		const auto ndc = [&](float x, float y) {
			return std::pair<float, float>{x / viewSizeX * 2.0f - 1.0f, y / viewSizeY * 2.0f - 1.0f};
		};
		const auto packedTint = [&](const float c[4]) {
			const float alpha = blendEnabled ? c[3] : 1.0f;
			return std::array<float, 4>{c[0], c[1], c[2], alpha};
		};

		const auto [x1, y1] = ndc(p1.first, p1.second);
		const auto [x2, y2] = ndc(p2.first, p2.second);
		const auto [x3, y3] = ndc(p3.first, p3.second);
		const auto tint1 = packedTint(color1);
		const auto tint2 = packedTint(color2);
		const auto tint3 = packedTint(color3);
		TextureVertex verts[3] = {
			{{x1, y1}, {u1, v1}, {tint1[0], tint1[1], tint1[2], tint1[3]}},
			{{x2, y2}, {u2, v2}, {tint2[0], tint2[1], tint2[2], tint2[3]}},
			{{x3, y3}, {u3, v3}, {tint3[0], tint3[1], tint3[2], tint3[3]}},
		};

		textureVertexBuffer->UpdateData(verts, sizeof(verts), 0);
		IShaderPipeline* pipeline = SelectTexturePipeline();
		pipeline->Enable();
		pipeline->BindTexture(0, *texture.texture);
		pipeline->BindVertexBuffer(0, *textureVertexBuffer);
		pipeline->Draw(PrimitiveTopology::Triangles, 0, 3);
		pipeline->Disable();
	}

	void DrawTextureScreen(const TextureCommandBuffer& texture, float x1, float y1, float x2, float y2,
	                       float u1, float v1, float u2, float v2)
	{
		DrawTextureScreenQuad(
			texture,
			{x1, y1},
			{x2, y1},
			{x1, y2},
			{x2, y2},
			u1, v1, u2, v2
		);
	}

	void DrawTextureScreenQuad(const TextureCommandBuffer& texture,
	                           const std::pair<float, float>& p00,
	                           const std::pair<float, float>& p10,
	                           const std::pair<float, float>& p01,
	                           const std::pair<float, float>& p11,
	                           float u1, float v1, float u2, float v2)
	{
		if (!valid || globalRendering == nullptr || texture.texture == nullptr || !texture.texture->IsValid())
			return;
		if (!ApplyScissor())
			return;

		if (texture.dirty) {
			texture.texture->UploadImage(texture.pixels.data());
			texture.dirty = false;
		}

		const float viewSizeX = std::max(1.0f, float(globalRendering->viewSizeX));
		const float viewSizeY = std::max(1.0f, float(globalRendering->viewSizeY));
		if (std::getenv("SPRING_METAL_LUAUI_TRACE") != nullptr && std::abs(p11.first - p00.first) > 1000.0f) {
			int srcNonzeroAlpha = 0;
			uint32_t srcMaxAlpha = 0;
			for (uint32_t p : texture.pixels) {
				const uint32_t a = (p >> 24) & 0xFFu;
				if (a > 0) ++srcNonzeroAlpha;
				if (a > srcMaxAlpha) srcMaxAlpha = a;
			}
			LOG_L(L_INFO, "[MetalLuaUI] draw texture quad view=%dx%d rect=(%.1f,%.1f)-(%.1f,%.1f) uv=(%.2f,%.2f)-(%.2f,%.2f) tex=%dx%d nzA=%d maxA=%u dirty=%d valid=%d blendEn=%d srcC=%u dstC=%u",
				globalRendering->viewSizeX, globalRendering->viewSizeY,
				p00.first, p00.second, p11.first, p11.second,
				u1, v1, u2, v2,
				texture.desc.width, texture.desc.height,
				srcNonzeroAlpha, srcMaxAlpha,
				texture.dirty ? 1 : 0,
				(texture.texture && texture.texture->IsValid()) ? 1 : 0,
				blendEnabled ? 1 : 0,
				blendSrcColor, blendDstColor);
		}
		const auto ndc = [&](float x, float y) {
			return std::pair<float, float>{x / viewSizeX * 2.0f - 1.0f, y / viewSizeY * 2.0f - 1.0f};
		};

		const auto [nx00, ny00] = ndc(p00.first, p00.second);
		const auto [nx10, ny10] = ndc(p10.first, p10.second);
		const auto [nx01, ny01] = ndc(p01.first, p01.second);
		const auto [nx11, ny11] = ndc(p11.first, p11.second);
		const float alpha = blendEnabled ? color[3] : 1.0f;
		const float tint[4] = {color[0], color[1], color[2], alpha};
		(void)tint;
		const float c1[4] = {0.0f, 0.0f, 1.0f, 1.0f};  // T1 blue
		const float c2[4] = {1.0f, 0.0f, 0.0f, 1.0f};  // T2 red
		// T1 expanded to a giant quad on the LEFT half so we can rule out
		// it being clipped by some near-degenerate hit; T2 stays normal.
		TextureVertex verts[6] = {
			{{-1.0f, -1.0f}, {0.0f, 0.0f}, {c1[0], c1[1], c1[2], c1[3]}},
			{{ 0.0f, -1.0f}, {1.0f, 0.0f}, {c1[0], c1[1], c1[2], c1[3]}},
			{{-1.0f,  1.0f}, {0.0f, 1.0f}, {c1[0], c1[1], c1[2], c1[3]}},
			{{nx01, ny01}, {u1, v2}, {c2[0], c2[1], c2[2], c2[3]}},
			{{nx10, ny10}, {u2, v1}, {c2[0], c2[1], c2[2], c2[3]}},
			{{nx11, ny11}, {u2, v2}, {c2[0], c2[1], c2[2], c2[3]}},
		};

		textureVertexBuffer->UpdateData(verts, sizeof(verts), 0);
		IShaderPipeline* pipeline = SelectTexturePipeline();
		pipeline->Enable();
		pipeline->BindTexture(0, *texture.texture);
		pipeline->BindVertexBuffer(0, *textureVertexBuffer);
		// Two separate draw calls per triangle. Drawing 6 verts in one
		// call was leaving the first triangle invisible on Apple Silicon
		// even with cullMode=None; isolating the draws sidesteps that.
		pipeline->Draw(PrimitiveTopology::Triangles, 0, 3);
		pipeline->Draw(PrimitiveTopology::Triangles, 3, 3);
		pipeline->Disable();
	}

	IShaderPipeline* SelectTexturePipeline() const
	{
		if (blendEnabled && blendSrcColor == GL_SRC_ALPHA && blendDstColor == GL_ONE)
			return textureAdditivePipeline.get();
		if (blendEnabled && blendSrcColor == GL_ONE && blendDstColor == GL_ONE_MINUS_SRC_ALPHA)
			return texturePremultipliedPipeline.get();

		return texturePipeline.get();
	}

	bool ApplyScissor() const
	{
		const int viewSizeX = std::max(1, globalRendering->viewSizeX);
		const int viewSizeY = std::max(1, globalRendering->viewSizeY);
		if (!scissorEnabled) {
			MetalGlobals::SetCurrentScissorRect(0, 0, viewSizeX, viewSizeY);
			return true;
		}

		const int x0 = std::clamp(scissorX, 0, viewSizeX);
		const int y0 = std::clamp(scissorY, 0, viewSizeY);
		const int x1 = std::clamp(scissorX + scissorW, 0, viewSizeX);
		const int y1 = std::clamp(scissorY + scissorH, 0, viewSizeY);
		const int width = std::max(0, x1 - x0);
		const int height = std::max(0, y1 - y0);
		if (width == 0 || height == 0)
			return false;

		MetalGlobals::SetCurrentScissorRect(
			static_cast<uint32_t>(x0),
			static_cast<uint32_t>(viewSizeY - y1),
			static_cast<uint32_t>(width),
			static_cast<uint32_t>(height)
		);
		return true;
	}

	std::unique_ptr<MetalTextOverlay> textOverlay;
	std::unique_ptr<IShaderPipeline> rectPipeline;
	std::unique_ptr<IShaderPipeline> texturePipeline;
	std::unique_ptr<IShaderPipeline> texturePremultipliedPipeline;
	std::unique_ptr<IShaderPipeline> textureAdditivePipeline;
	std::unique_ptr<IBuffer> rectVertexBuffer;
	std::unique_ptr<IBuffer> textureVertexBuffer;
	std::unordered_map<int, TextureCommandBuffer> textures;
	std::unordered_map<std::string, int> namedTextureIDs;
	std::unordered_map<int, std::vector<ListCommand>> lists;
	std::vector<Affine2D> matrixStack;
	std::array<float, 4> color = {1.0f, 1.0f, 1.0f, 1.0f};
	int nextTextureID = 1;
	int nextListID = 1;
	int capturingTexture = 0;
	int capturingList = 0;
	int boundTexture = 0;
	int scissorX = 0;
	int scissorY = 0;
	int scissorW = 0;
	int scissorH = 0;
	bool scissorEnabled = false;
	bool blendEnabled = true;
	uint32_t blendSrcColor = GL_SRC_ALPHA;
	uint32_t blendDstColor = GL_ONE_MINUS_SRC_ALPHA;
	uint32_t blendSrcAlpha = GL_ONE;
	uint32_t blendDstAlpha = GL_ONE_MINUS_SRC_ALPHA;
	bool valid = false;
};

Renderer& GetRenderer()
{
	static Renderer renderer;
	return renderer;
}

} // namespace

namespace MetalLuaUI
{
	void Init() { GetRenderer().Init(); }
	void Kill() { GetRenderer().Kill(); }
	void BeginFrame() { GetRenderer().BeginFrame(); }
	int CreateTexture(int width, int height) { return GetRenderer().CreateTexture(width, height); }
	void DeleteTexture(int textureID) { GetRenderer().DeleteTexture(textureID); }
	void BindTexture(int textureID) { GetRenderer().BindTexture(textureID); }
	void BindNamedTexture(const std::string& name) { GetRenderer().BindNamedTexture(name); }
	void UnbindTexture() { GetRenderer().UnbindTexture(); }
	bool HasBoundTexture() { return GetRenderer().HasBoundTexture(); }
	void RenderToTexture(int textureID, const std::function<void()>& drawFunc) { GetRenderer().RenderToTexture(textureID, drawFunc); }
	void Clear(float r, float g, float b, float a) { GetRenderer().Clear(r, g, b, a); }
	bool GetCaptureTextureSize(int& width, int& height) { return GetRenderer().GetCaptureTextureSize(width, height); }
	bool IsCapturing() { return GetRenderer().IsCapturing(); }
	int CreateList(const std::function<void()>& drawFunc) { return GetRenderer().CreateList(drawFunc); }
	void DeleteList(int listID) { GetRenderer().DeleteList(listID); }
	void CallList(int listID) { GetRenderer().CallList(listID); }
	void PushMatrix() { GetRenderer().PushMatrix(); }
	void PopMatrix() { GetRenderer().PopMatrix(); }
	void LoadIdentity() { GetRenderer().LoadIdentity(); }
	void Ortho(float left, float right, float bottom, float top, float, float) { GetRenderer().Ortho(left, right, bottom, top); }
	void Translate(float x, float y, float) { GetRenderer().Translate(x, y); }
	void Rotate(float degrees, float, float, float) { GetRenderer().Rotate(degrees); }
	void Scale(float x, float y, float) { GetRenderer().Scale(x, y); }
	void SetScissor(bool enabled, int x, int y, int width, int height) { GetRenderer().SetScissor(enabled, x, y, width, height); }
	void SetBlending(bool enabled) { GetRenderer().SetBlending(enabled); }
	void SetBlendFunc(uint32_t src, uint32_t dst) { GetRenderer().SetBlendFunc(src, dst); }
	void SetBlendFuncSeparate(uint32_t srcColor, uint32_t dstColor, uint32_t srcAlpha, uint32_t dstAlpha) { GetRenderer().SetBlendFuncSeparate(srcColor, dstColor, srcAlpha, dstAlpha); }
	void SetColor(float r, float g, float b, float a) { GetRenderer().SetColor(r, g, b, a); }
	void DrawText(const LuaUITextDraw& text) { GetRenderer().DrawText(text); }
	void DrawRect(float x1, float y1, float x2, float y2) { GetRenderer().DrawRect(x1, y1, x2, y2); }
	void DrawTriangle(float x1, float y1, float x2, float y2, float x3, float y3) { GetRenderer().DrawTriangle(x1, y1, x2, y2, x3, y3); }
	void DrawTriangleColored(
		float x1, float y1, const float color1[4],
		float x2, float y2, const float color2[4],
		float x3, float y3, const float color3[4])
	{
		GetRenderer().DrawTriangleColored(x1, y1, color1, x2, y2, color2, x3, y3, color3);
	}
	void DrawBoundTexturedTriangle(
		float x1, float y1, float u1, float v1, const float color1[4],
		float x2, float y2, float u2, float v2, const float color2[4],
		float x3, float y3, float u3, float v3, const float color3[4])
	{
		GetRenderer().DrawBoundTexturedTriangle(
			x1, y1, u1, v1, color1,
			x2, y2, u2, v2, color2,
			x3, y3, u3, v3, color3);
	}
	void DrawBoundTextureRect(float x1, float y1, float x2, float y2) { GetRenderer().DrawBoundTextureRect(x1, y1, x2, y2); }
	void DrawBoundTextureRectUV(float x1, float y1, float x2, float y2, float u1, float v1, float u2, float v2) { GetRenderer().DrawBoundTextureRectUV(x1, y1, x2, y2, u1, v1, u2, v2); }
}

#endif // RENDER_BACKEND_METAL

