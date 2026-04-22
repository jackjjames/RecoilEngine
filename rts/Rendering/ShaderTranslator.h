/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Shader {

enum class Stage {
	Vertex,
	Fragment,
};

struct TranslateResult {
	bool ok = false;
	std::string msl;
	std::string log;
};

// Compile a GLSL source string (Vulkan semantics) to SPIR-V, then cross-compile
// to MSL targeting macOS 2.3. Emits a human-readable log on failure.
//
// This function is only built for APPLE + SPRING_RENDER_BACKEND=metal. GL
// builds never see it.
TranslateResult TranslateGlslToMsl(Stage stage, const std::string& source, const std::string& entryPoint = "main");

// Same thing, but accepts pre-compiled SPIR-V. Useful for shader caches.
TranslateResult TranslateSpirvToMsl(Stage stage, const std::vector<uint32_t>& spirv, const std::string& entryPoint = "main");

} // namespace Shader
