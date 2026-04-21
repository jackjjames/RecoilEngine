/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef LUA_OPENGLUTILS_H
#define LUA_OPENGLUTILS_H

#include <string>

#include "Lua/LuaMatTexture.h"
#include "System/type2.h"

class CMatrix44f;
struct lua_State;

enum LuaMatrixType {
	LUAMATRICES_SHADOW,
	LUAMATRICES_VIEW,
	LUAMATRICES_VIEWINVERSE,
	LUAMATRICES_PROJECTION,
	LUAMATRICES_PROJECTIONINVERSE,
	LUAMATRICES_VIEWPROJECTION,
	LUAMATRICES_VIEWPROJECTIONINVERSE,
	LUAMATRICES_BILLBOARD,
	LUAMATRICES_NONE
};

class LuaOpenGLUtils {
public:
	static void ResetState();

	static const CMatrix44f* GetNamedMatrix(const char* name);

	static LuaMatTexture::Type GetLuaMatTextureType(const std::string& name);
	static LuaMatrixType GetLuaMatrixType(const char* name);

	static bool ParseTextureImage(lua_State* L, LuaMatTexture& texUnit, const std::string& image);
};

#endif // LUA_OPENGLUTILS_H
