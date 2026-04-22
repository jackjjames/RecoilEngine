/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "VAO.h"
#include "Rendering/GL/myGL.h"
#include "System/Misc/TracyDefs.h"


bool VAO::IsSupported()
{
	RECOIL_DETAILED_TRACY_ZONE;
#if defined(RENDER_BACKEND_METAL)
	// GL vertex-array objects have no Metal equivalent; vertex layout lives
	// in MTLVertexDescriptor attached to the render-pipeline state (see
	// IShaderPipeline::PipelineDesc). Declare unsupported so VAO wrappers
	// across the engine short-circuit to a no-op; the real path is per
	// subsystem during Stage 9.
	return false;
#else
	static bool supported = GLAD_GL_ARB_vertex_array_object;
	return supported;
#endif
}

void VAO::Generate() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (id > 0)
		return;

#if defined(RENDER_BACKEND_METAL)
	return;
#else
	glGenVertexArrays(1, &id);
#endif
}

void VAO::Delete() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (id > 0) {
#if !defined(RENDER_BACKEND_METAL)
		glDeleteVertexArrays(1, &id);
#endif
		id = 0;
	}
}

void VAO::Bind() const
{
	RECOIL_DETAILED_TRACY_ZONE;
#if defined(RENDER_BACKEND_METAL)
	return;
#else
	glBindVertexArray(GetId());
#endif
}

void VAO::Unbind() const
{
	RECOIL_DETAILED_TRACY_ZONE;
#if defined(RENDER_BACKEND_METAL)
	return;
#else
	glBindVertexArray(0);
#endif
}
