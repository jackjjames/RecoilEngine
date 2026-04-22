/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Rendering/IBuffer.h"

namespace Shader {
	struct IShaderObject;
}

class ITexture;

struct ShaderReflectionEntry
{
	std::string name;
	uint32_t binding = 0;
};

struct ShaderReflection
{
	std::vector<ShaderReflectionEntry> uniformBuffers;
};

enum class VertexFormat
{
	Float1,
	Float2,
	Float3,
	Float4,
};

// Single vertex attribute. `location` is the GLSL `layout(location = N)` slot
// and matches `[[attribute(N)]]` on the translated MSL side. `bufferSlot` is
// the vertex buffer index this attribute reads from (allows interleaved or
// separate-stream layouts).
struct VertexAttribute
{
	uint32_t location   = 0;
	uint32_t bufferSlot = 0;
	uint32_t offset     = 0;
	VertexFormat format = VertexFormat::Float4;
};

// Per vertex-buffer-slot layout. Today only per-vertex is supported; per-
// instance advances later when it is needed.
struct VertexBindingLayout
{
	uint32_t slot    = 0;
	uint32_t stride  = 0;
};

enum class IndexType
{
	Uint16,
	Uint32,
};

struct PipelineDesc
{
	std::string name;
	std::string vertexSource;
	std::string fragmentSource;

	// Optional vertex input description. Leave empty for vertex-less
	// procedural pipelines (gl_VertexIndex style). The translator still
	// accepts `layout(location = N) in ...` declarations; the entries here
	// just describe how the GPU decodes the bound vertex buffer(s).
	std::vector<VertexAttribute> vertexAttributes;
	std::vector<VertexBindingLayout> vertexBindings;

	// Metal-only: which MTL buffer index the vertex-stage should read vertex
	// data from. spirv-cross defaults to slot 30 for a single interleaved
	// stream; when callers use multiple streams the GLSL `layout(location = ...)`
	// declarations still drive attribute routing but the buffer slot is
	// chosen here. Matches `slot` inside VertexBindingLayout; ignored on GL.
	uint32_t metalVertexBufferBaseSlot = 30;
};

enum class PrimitiveTopology
{
	Triangles,
	TriangleStrip,
	Lines,
	LineStrip,
	Points,
};

class IShaderPipeline
{
public:
	virtual ~IShaderPipeline() = default;

	virtual void BindAttribLocation(const std::string& name, uint32_t index) = 0;
	virtual void BindOutputLocation(const std::string& name, uint32_t index) = 0;
	virtual void Enable() = 0;
	virtual void Disable() = 0;
	virtual void Link() = 0;
	virtual bool Validate() = 0;
	virtual void Release() = 0;
	virtual void Reload(bool reloadFromDisk, bool validate) = 0;
	virtual void AttachShaderObject(Shader::IShaderObject* so) = 0;

	virtual const std::string& GetName() const = 0;
	virtual const std::string& GetLog() const = 0;
	virtual bool IsValid() const = 0;
	virtual void BindUniformBuffer(uint32_t slot, const IBuffer& buffer, size_t offset, size_t size)
	{
		buffer.BindUniformRange(slot, offset, size);
	}
	virtual void SetPushConstants(const void*, size_t) {}
	virtual const ShaderReflection& GetReflection() const
	{
		static const ShaderReflection reflection;
		return reflection;
	}

	// Bind a vertex buffer range. `slot` corresponds to the VertexBindingLayout
	// slot specified in PipelineDesc. Default is a no-op so legacy program
	// objects that do not participate in the vertex input pipeline still
	// compile. Backend standalone pipelines override.
	virtual void BindVertexBuffer(uint32_t /*slot*/, const IBuffer& /*buffer*/, size_t /*offset*/ = 0) {}

	// Bind a texture sampled-image to the fragment stage. Slot indexing is
	// shared with Metal's `[[texture(N)]]` and GL's sampler unit index.
	virtual void BindTexture(uint32_t /*slot*/, ITexture& /*texture*/) {}

	// Issue a draw against the currently-enabled pipeline. Callers are
	// expected to have called Enable() and any BindUniformBuffer() first.
	// `firstVertex`/`vertexCount` follow glDrawArrays semantics.
	//
	// Default is a no-op so legacy engine shader program objects that live
	// through IProgramObject do not need to implement it. Backend standalone
	// pipelines (GL + Metal) override this.
	virtual void Draw(PrimitiveTopology /*topology*/, uint32_t /*firstVertex*/, uint32_t /*vertexCount*/) {}

	// Indexed draw. `indexBuffer` is bound + consumed at draw time.
	// `indexOffset` is in bytes from the start of the index buffer.
	virtual void DrawIndexed(PrimitiveTopology /*topology*/, uint32_t /*indexCount*/,
	                         IndexType /*indexType*/, const IBuffer& /*indexBuffer*/,
	                         size_t /*indexOffset*/ = 0) {}
};

class GLShaderPipeline : public IShaderPipeline
{
public:
	virtual unsigned int GetObjID() const = 0;

	// GL-only convenience for legacy consumers that still need a raw
	// glGetUniformLocation. Returns -1 by default so this is safe to call
	// through the base pointer even when other backends live at the other
	// end. Named LookupUniformLocation to avoid clashing with the existing
	// private pure-virtual int GetUniformLoc(const char*) on IProgramObject.
	virtual int LookupUniformLocation(const char* /*name*/) const { return -1; }
};
