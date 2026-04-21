/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

struct LuaGLCapabilities
{
	bool depthClamp = false;
	bool alphaToCoverage = false;
	bool blendEquationSeparate = false;
	bool blendFuncSeparate = false;
	bool stencilTwoSide = false;
	bool framebuffer = false;
	bool generateMipmap = false;
	bool occlusionQuery = false;
	bool khrDebug = false;
	bool shaders = false;
	bool legacyImmediate = false;
	bool legacyMatrix = false;
	bool legacyLighting = false;
	bool displayLists = false;
	bool computeShader = false;
};
