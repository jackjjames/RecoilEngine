/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>
#include <string>

struct LuaUITextDraw
{
	std::string text;
	float x = 0.0f;
	float y = 0.0f;
	float size = 0.0f;
	float fontSize = 0.0f;
	int options = 0;
};

struct LuaUIRectDraw
{
	float x1 = 0.0f;
	float y1 = 0.0f;
	float x2 = 0.0f;
	float y2 = 0.0f;
	float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

struct LuaUITextureDesc
{
	int width = 0;
	int height = 0;
};

