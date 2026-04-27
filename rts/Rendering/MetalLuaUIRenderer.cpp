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
	bool dirty = false;
};

struct ListCommand {
	enum class Type {
		Text,
		Rect,
		Texture,
		BindTexture,
		Color,
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
	float u1 = 0.0f;
	float v1 = 0.0f;
	float u2 = 1.0f;
	float v2 = 1.0f;
	float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
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
    fragColor = texture(uTex, vUV) * vColor;
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
		valid = texturePipeline && texturePipeline->IsValid();
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
		texturePipeline.reset();
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

		auto textureHandle = bitmap.CreateTextureHandle(tcp);
		if (textureHandle == nullptr || !textureHandle->IsValid()) {
			UnbindTexture();
			return false;
		}

		const int textureID = nextTextureID++;
		auto& texture = textures[textureID];
		texture.desc.width = std::max(1, bitmap.xsize);
		texture.desc.height = std::max(1, bitmap.ysize);
		texture.texture = std::move(textureHandle);
		if (const uint8_t* bitmapPixels = bitmap.GetRawMem(); bitmapPixels != nullptr) {
			texture.pixels.resize(texture.desc.width * texture.desc.height);
			std::copy_n(reinterpret_cast<const uint32_t*>(bitmapPixels), texture.pixels.size(), texture.pixels.begin());
		}
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

		const int previousCapture = capturingTexture;
		const auto previousStack = matrixStack;
		capturingTexture = textureID;
		matrixStack.clear();
		matrixStack.push_back(Affine2D{});

		drawFunc();

		matrixStack = previousStack;
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

	int CreateList(const std::function<void()>& drawFunc)
	{
		const int listID = nextListID++;
		auto& list = lists[listID];
		list.clear();

		const int previousList = capturingList;
		const auto previousStack = matrixStack;
		const auto previousColor = color;
		const int previousTexture = boundTexture;
		capturingList = listID;
		drawFunc();
		capturingList = previousList;
		matrixStack = previousStack;
		color = previousColor;
		boundTexture = previousTexture;

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
				case ListCommand::Type::Texture: {
					const auto previousColor = color;
					const int previousTexture = boundTexture;
					std::copy(command.color, command.color + 4, color.begin());
					if (command.textureID != 0)
						boundTexture = command.textureID;
					DrawBoundTextureRectUV(command.x1, command.y1, command.x2, command.y2, command.u1, command.v1, command.u2, command.v2);
					boundTexture = previousTexture;
					color = previousColor;
				} break;
				case ListCommand::Type::BindTexture: {
					boundTexture = command.textureID;
				} break;
				case ListCommand::Type::Color: {
					std::copy(command.color, command.color + 4, color.begin());
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

	void DrawBoundTextureRect(float x1, float y1, float x2, float y2)
	{
		DrawBoundTextureRectUV(x1, y1, x2, y2, 0.0f, 1.0f, 1.0f, 0.0f);
	}

	void DrawBoundTextureRectUV(float x1, float y1, float x2, float y2, float u1, float v1, float u2, float v2)
	{
		if (capturingList != 0) {
			ListCommand command;
			command.type = ListCommand::Type::Texture;
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

		const auto it = textures.find(boundTexture);
		if (it == textures.end())
			return;

		const auto& texture = it->second;
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
			LuaUITextDraw draw = text.draw;
			draw.x = p00.first + (text.localX / texture.desc.width) * (p11.first - p00.first);
			draw.y = p00.second + (text.localY / texture.desc.height) * (p11.second - p00.second);
			draw.size = text.localSize * ((p11.second - p00.second) / texture.desc.height);
			DrawTextScreen(draw, p11.first);
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

	static uint32_t BlendOver(uint32_t dst, uint32_t src)
	{
		const uint32_t srcA = (src >> 24) & 0xFFu;
		if (srcA == 0)
			return dst;
		if (srcA == 0xFFu)
			return src;

		const uint32_t invA = 255u - srcA;
		const uint32_t r = (((src        & 0xFFu) * srcA) + ((dst        & 0xFFu) * invA)) / 255u;
		const uint32_t g = ((((src >>  8) & 0xFFu) * srcA) + (((dst >>  8) & 0xFFu) * invA)) / 255u;
		const uint32_t b = ((((src >> 16) & 0xFFu) * srcA) + (((dst >> 16) & 0xFFu) * invA)) / 255u;
		const uint32_t a = std::min(255u, srcA + (((dst >> 24) & 0xFFu) * invA) / 255u);
		return r | (g << 8) | (b << 16) | (a << 24);
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
			std::fill(row + x0, row + x1, packed);
		}
		texture.dirty = true;
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
			const int srcY = std::clamp(int(v * float(source.desc.height - 1) + 0.5f), 0, source.desc.height - 1);
			const uint32_t* srcRow = source.pixels.data() + srcY * source.desc.width;
			for (int x = x0; x < x1; ++x) {
				const float u = u1 + (u2 - u1) * std::clamp(((float(x) + 0.5f) - localX1) * invWidth, 0.0f, 1.0f);
				const int srcX = std::clamp(int(u * float(source.desc.width - 1) + 0.5f), 0, source.desc.width - 1);
				const uint32_t src = TintPixel(srcRow[srcX], color, blendEnabled);
				dstRow[x] = BlendOver(dstRow[x], src);
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

		if (draw.options & (FONT_CENTER | FONT_RIGHT)) {
			const float aspect = globalRendering->aspectRatio > 0.0f ? globalRendering->aspectRatio : 1.0f;
			const float textWidth = glyphH * (5.0f / 7.0f) * 1.2f * float(draw.text.size()) / aspect;
			if (draw.options & FONT_CENTER)
				ndcX -= textWidth * 0.5f;
			else if (draw.options & FONT_RIGHT)
				ndcX -= textWidth;
		}

		if (draw.options & FONT_TOP)
			ndcTop = normY * 2.0f - 1.0f;
		else if (draw.options & FONT_VCENTER)
			ndcTop = normY * 2.0f - 1.0f + glyphH * 0.5f;

		const float ndcMaxX = (maxX / viewSizeX) * 2.0f - 1.0f;
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

		if (texture.dirty)
			texture.texture->UploadImage(texture.pixels.data());

		const float viewSizeX = std::max(1.0f, float(globalRendering->viewSizeX));
		const float viewSizeY = std::max(1.0f, float(globalRendering->viewSizeY));
		const auto ndc = [&](float x, float y) {
			return std::pair<float, float>{x / viewSizeX * 2.0f - 1.0f, y / viewSizeY * 2.0f - 1.0f};
		};

		const auto [nx00, ny00] = ndc(p00.first, p00.second);
		const auto [nx10, ny10] = ndc(p10.first, p10.second);
		const auto [nx01, ny01] = ndc(p01.first, p01.second);
		const auto [nx11, ny11] = ndc(p11.first, p11.second);
		const float alpha = blendEnabled ? color[3] : 1.0f;
		const float tint[4] = {color[0], color[1], color[2], alpha};
		TextureVertex verts[6] = {
			{{nx00, ny00}, {u1, v1}, {tint[0], tint[1], tint[2], tint[3]}},
			{{nx10, ny10}, {u2, v1}, {tint[0], tint[1], tint[2], tint[3]}},
			{{nx01, ny01}, {u1, v2}, {tint[0], tint[1], tint[2], tint[3]}},
			{{nx01, ny01}, {u1, v2}, {tint[0], tint[1], tint[2], tint[3]}},
			{{nx10, ny10}, {u2, v1}, {tint[0], tint[1], tint[2], tint[3]}},
			{{nx11, ny11}, {u2, v2}, {tint[0], tint[1], tint[2], tint[3]}},
		};

		textureVertexBuffer->UpdateData(verts, sizeof(verts), 0);
		texturePipeline->Enable();
		texturePipeline->BindTexture(0, *texture.texture);
		texturePipeline->BindVertexBuffer(0, *textureVertexBuffer);
		texturePipeline->Draw(PrimitiveTopology::Triangles, 0, 6);
		texturePipeline->Disable();
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
	void SetColor(float r, float g, float b, float a) { GetRenderer().SetColor(r, g, b, a); }
	void DrawText(const LuaUITextDraw& text) { GetRenderer().DrawText(text); }
	void DrawRect(float x1, float y1, float x2, float y2) { GetRenderer().DrawRect(x1, y1, x2, y2); }
	void DrawBoundTextureRect(float x1, float y1, float x2, float y2) { GetRenderer().DrawBoundTextureRect(x1, y1, x2, y2); }
	void DrawBoundTextureRectUV(float x1, float y1, float x2, float y2, float u1, float v1, float u2, float v2) { GetRenderer().DrawBoundTextureRectUV(x1, y1, x2, y2, u1, v1, u2, v2); }
}

#endif // RENDER_BACKEND_METAL

