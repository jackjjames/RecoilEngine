# Fetches glslang + SPIRV-Cross for the Metal backend's runtime GLSL -> SPIR-V -> MSL
# translation path. Only included on APPLE + SPRING_RENDER_BACKEND=metal so the GL
# build never pays the compile cost.

include(FetchContent)

set(GLSLANG_TAG "15.0.0" CACHE STRING "glslang tag/commit to fetch")
set(SPIRV_CROSS_TAG "vulkan-sdk-1.3.296.0" CACHE STRING "SPIRV-Cross tag/commit to fetch")

# glslang: we only need the translator itself + SPIRV emission. No optimizer,
# no tests, no binaries. This keeps the dependency tree flat (no SPIRV-Tools).
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
	glslang
	GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
	GIT_TAG        ${GLSLANG_TAG}
	GIT_SHALLOW    TRUE
)

# SPIRV-Cross: we only need the core + MSL backend. Skip CLI, tests, and the
# other language backends to keep link time down.
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL ON  CACHE BOOL "" FORCE)  # MSL backend depends on GLSL backend
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL  ON  CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP  OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL ON  CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)

FetchContent_Declare(
	spirv_cross
	GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
	GIT_TAG        ${SPIRV_CROSS_TAG}
	GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(glslang spirv_cross)

set(SPRING_SHADER_TRANSLATOR_LIBS
	glslang
	SPIRV
	spirv-cross-core
	spirv-cross-glsl
	spirv-cross-msl
	CACHE INTERNAL "Link libs needed for the GLSL -> MSL translator"
)
