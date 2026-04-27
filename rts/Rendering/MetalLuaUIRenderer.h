/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <cstdint>
#include <functional>
#include <string>

#include "Rendering/LuaUIRenderer.h"

namespace MetalLuaUI
{
	void Init();
	void Kill();
	void BeginFrame();

	int CreateTexture(int width, int height);
	void DeleteTexture(int textureID);
	void BindTexture(int textureID);
	void BindNamedTexture(const std::string& name);
	void UnbindTexture();
	bool HasBoundTexture();
	void RenderToTexture(int textureID, const std::function<void()>& drawFunc);
	void Clear(float r, float g, float b, float a);
	bool GetCaptureTextureSize(int& width, int& height);
	// True while inside gl.RenderToTexture or gl.CreateList; immediate-mode
	// geometry (gl.BeginEnd, gl.Shape) must be captured into the active target.
	bool IsCapturing();
	int CreateList(const std::function<void()>& drawFunc);
	void DeleteList(int listID);
	void CallList(int listID);

	void PushMatrix();
	void PopMatrix();
	void LoadIdentity();
	void Ortho(float left, float right, float bottom, float top, float zNear = -1.0f, float zFar = 1.0f);
	void Translate(float x, float y, float z = 0.0f);
	void Rotate(float degrees, float x = 0.0f, float y = 0.0f, float z = 1.0f);
	void Scale(float x, float y, float z = 1.0f);

	void SetScissor(bool enabled, int x = 0, int y = 0, int width = 0, int height = 0);
	void SetBlending(bool enabled);
	void SetBlendFunc(uint32_t src, uint32_t dst);
	void SetBlendFuncSeparate(uint32_t srcColor, uint32_t dstColor, uint32_t srcAlpha, uint32_t dstAlpha);
	void SetColor(float r, float g, float b, float a);
	void DrawText(const LuaUITextDraw& text);
	void DrawRect(float x1, float y1, float x2, float y2);
	void DrawTriangle(float x1, float y1, float x2, float y2, float x3, float y3);
	// Triangle drawn with per-vertex RGBA. Colors interpolate barycentrically
	// during rasterization so feather outlines (RectRoundOutline) and gradient
	// fills (RectRound with colorTop / colorBottom) reproduce correctly.
	void DrawTriangleColored(
		float x1, float y1, const float color1[4],
		float x2, float y2, const float color2[4],
		float x3, float y3, const float color3[4]);
	void DrawBoundTextureRect(float x1, float y1, float x2, float y2);
	void DrawBoundTextureRectUV(float x1, float y1, float x2, float y2, float u1, float v1, float u2, float v2);
}

#endif // RENDER_BACKEND_METAL

