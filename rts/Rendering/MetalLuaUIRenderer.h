/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

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
	void UnbindTexture();
	void RenderToTexture(int textureID, const std::function<void()>& drawFunc);
	int CreateList(const std::function<void()>& drawFunc);
	void DeleteList(int listID);
	void CallList(int listID);

	void PushMatrix();
	void PopMatrix();
	void Translate(float x, float y, float z = 0.0f);
	void Scale(float x, float y, float z = 1.0f);

	void SetScissor(bool enabled, int x = 0, int y = 0, int width = 0, int height = 0);
	void SetBlending(bool enabled);
	void SetColor(float r, float g, float b, float a);
	void DrawText(const LuaUITextDraw& text);
	void DrawRect(float x1, float y1, float x2, float y2);
	void DrawBoundTextureRect(float x1, float y1, float x2, float y2);
}

#endif // RENDER_BACKEND_METAL

