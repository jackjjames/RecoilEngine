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

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
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
};

struct CapturedText {
	LuaUITextDraw draw;
	float localX = 0.0f;
	float localY = 0.0f;
	float localSize = 0.0f;
};

struct CapturedRect {
	LuaUIRectDraw draw;
	float localX1 = 0.0f;
	float localY1 = 0.0f;
	float localX2 = 0.0f;
	float localY2 = 0.0f;
};

struct TextureCommandBuffer {
	LuaUITextureDesc desc;
	std::vector<CapturedText> texts;
	std::vector<CapturedRect> rects;
};

struct ListCommand {
	enum class Type {
		Text,
		Rect,
	};

	Type type = Type::Rect;
	LuaUITextDraw text;
	LuaUIRectDraw rect;
};

struct RectVertex {
	float pos[2];
	float color[4];
};

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

		PipelineDesc pd;
		pd.name = "lua_ui_rect";
		pd.vertexSource = kRectVertexGlsl;
		pd.fragmentSource = kRectFragmentGlsl;
		pd.vertexAttributes = {
			VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = 0, .format = VertexFormat::Float2 },
			VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = sizeof(float) * 2, .format = VertexFormat::Float4 },
		};
		pd.vertexBindings = {
			VertexBindingLayout{ .slot = 0, .stride = sizeof(RectVertex) },
		};
		pd.blendState = RenderTargetBlendState{
			.enabled = true,
			.srcColor = GL_SRC_ALPHA,
			.dstColor = GL_ONE_MINUS_SRC_ALPHA,
			.srcAlpha = GL_ONE,
			.dstAlpha = GL_ONE_MINUS_SRC_ALPHA,
		};
		pipeline = backend.CreatePipeline(pd);
		valid = pipeline && pipeline->IsValid();
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
		scissorEnabled = false;
		blendEnabled = true;
		pipeline.reset();
		rectVertexBuffer.reset();
		textOverlay.reset();
		valid = false;
	}

	void BeginFrame()
	{
		if (textOverlay != nullptr)
			textOverlay->BeginFrame();
	}

	int CreateTexture(int width, int height)
	{
		const int textureID = nextTextureID++;
		auto& texture = textures[textureID];
		texture.desc.width = std::max(1, width);
		texture.desc.height = std::max(1, height);
		return textureID;
	}

	void DeleteTexture(int textureID)
	{
		textures.erase(textureID);
		if (boundTexture == textureID)
			boundTexture = 0;
	}

	void BindTexture(int textureID) { boundTexture = textureID; }
	void UnbindTexture() { boundTexture = 0; }

	void RenderToTexture(int textureID, const std::function<void()>& drawFunc)
	{
		auto it = textures.find(textureID);
		if (it == textures.end())
			return;

		it->second.texts.clear();
		it->second.rects.clear();

		const int previousCapture = capturingTexture;
		const auto previousStack = matrixStack;
		capturingTexture = textureID;
		matrixStack.clear();
		matrixStack.push_back(Affine2D{});

		drawFunc();

		matrixStack = previousStack;
		capturingTexture = previousCapture;
	}

	int CreateList(const std::function<void()>& drawFunc)
	{
		const int listID = nextListID++;
		auto& list = lists[listID];
		list.clear();

		const int previousList = capturingList;
		capturingList = listID;
		drawFunc();
		capturingList = previousList;

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
			if (command.type == ListCommand::Type::Text) {
				DrawText(command.text);
			} else {
				const auto previousColor = color;
				std::copy(command.rect.color, command.rect.color + 4, color.begin());
				DrawRect(command.rect.x1, command.rect.y1, command.rect.x2, command.rect.y2);
				color = previousColor;
			}
		}
	}

	void PushMatrix()
	{
		matrixStack.push_back(CurrentMatrix());
	}

	void PopMatrix()
	{
		if (matrixStack.size() > 1)
			matrixStack.pop_back();
	}

	void Translate(float x, float y)
	{
		CurrentMatrix().PostTranslate(x, y);
	}

	void Scale(float x, float y)
	{
		CurrentMatrix().PostScale(x, y);
	}

	void SetColor(float r, float g, float b, float a)
	{
		color = {r, g, b, a};
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

		DrawRectScreen(rect);
	}

	void DrawBoundTextureRect(float x1, float y1, float x2, float y2)
	{
		const auto it = textures.find(boundTexture);
		if (it == textures.end())
			return;

		const auto& texture = it->second;

		for (const CapturedRect& rect: texture.rects) {
			LuaUIRectDraw draw = rect.draw;
			draw.x1 = x1 + (rect.localX1 / texture.desc.width) * (x2 - x1);
			draw.y1 = y1 + (rect.localY1 / texture.desc.height) * (y2 - y1);
			draw.x2 = x1 + (rect.localX2 / texture.desc.width) * (x2 - x1);
			draw.y2 = y1 + (rect.localY2 / texture.desc.height) * (y2 - y1);
			DrawRectScreen(draw);
		}

		for (const CapturedText& text: texture.texts) {
			LuaUITextDraw draw = text.draw;
			draw.x = x1 + (text.localX / texture.desc.width) * (x2 - x1);
			draw.y = y1 + (text.localY / texture.desc.height) * (y2 - y1);
			draw.size = text.localSize * ((y2 - y1) / texture.desc.height);
			DrawTextScreen(draw, x2);
		}
	}

private:
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

		CapturedRect captured;
		captured.draw = rect;
		captured.localX1 = localX1;
		captured.localY1 = localY1;
		captured.localX2 = localX2;
		captured.localY2 = localY2;
		it->second.rects.push_back(std::move(captured));
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
		if (!valid || globalRendering == nullptr)
			return;
		if (!ApplyScissor())
			return;

		const float viewSizeX = std::max(1.0f, float(globalRendering->viewSizeX));
		const float viewSizeY = std::max(1.0f, float(globalRendering->viewSizeY));
		const auto ndc = [&](float x, float y) {
			return std::pair<float, float>{x / viewSizeX * 2.0f - 1.0f, y / viewSizeY * 2.0f - 1.0f};
		};

		const auto [x0, y0] = ndc(rect.x1, rect.y1);
		const auto [x1, y1] = ndc(rect.x2, rect.y2);
		const float alpha = blendEnabled ? rect.color[3] : 1.0f;
		RectVertex verts[6] = {
			{{x0, y0}, {rect.color[0], rect.color[1], rect.color[2], alpha}},
			{{x1, y0}, {rect.color[0], rect.color[1], rect.color[2], alpha}},
			{{x0, y1}, {rect.color[0], rect.color[1], rect.color[2], alpha}},
			{{x0, y1}, {rect.color[0], rect.color[1], rect.color[2], alpha}},
			{{x1, y0}, {rect.color[0], rect.color[1], rect.color[2], alpha}},
			{{x1, y1}, {rect.color[0], rect.color[1], rect.color[2], alpha}},
		};

		rectVertexBuffer->UpdateData(verts, sizeof(verts), 0);
		pipeline->Enable();
		pipeline->BindVertexBuffer(0, *rectVertexBuffer);
		pipeline->Draw(PrimitiveTopology::Triangles, 0, 6);
		pipeline->Disable();
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
	std::unique_ptr<IShaderPipeline> pipeline;
	std::unique_ptr<IBuffer> rectVertexBuffer;
	std::unordered_map<int, TextureCommandBuffer> textures;
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
	void UnbindTexture() { GetRenderer().UnbindTexture(); }
	void RenderToTexture(int textureID, const std::function<void()>& drawFunc) { GetRenderer().RenderToTexture(textureID, drawFunc); }
	int CreateList(const std::function<void()>& drawFunc) { return GetRenderer().CreateList(drawFunc); }
	void DeleteList(int listID) { GetRenderer().DeleteList(listID); }
	void CallList(int listID) { GetRenderer().CallList(listID); }
	void PushMatrix() { GetRenderer().PushMatrix(); }
	void PopMatrix() { GetRenderer().PopMatrix(); }
	void Translate(float x, float y, float) { GetRenderer().Translate(x, y); }
	void Scale(float x, float y, float) { GetRenderer().Scale(x, y); }
	void SetScissor(bool enabled, int x, int y, int width, int height) { GetRenderer().SetScissor(enabled, x, y, width, height); }
	void SetBlending(bool enabled) { GetRenderer().SetBlending(enabled); }
	void SetColor(float r, float g, float b, float a) { GetRenderer().SetColor(r, g, b, a); }
	void DrawText(const LuaUITextDraw& text) { GetRenderer().DrawText(text); }
	void DrawRect(float x1, float y1, float x2, float y2) { GetRenderer().DrawRect(x1, y1, x2, y2); }
	void DrawBoundTextureRect(float x1, float y1, float x2, float y2) { GetRenderer().DrawBoundTextureRect(x1, y1, x2, y2); }
}

#endif // RENDER_BACKEND_METAL

