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

struct ShaderReflectionEntry
{
	std::string name;
	uint32_t binding = 0;
};

struct ShaderReflection
{
	std::vector<ShaderReflectionEntry> uniformBuffers;
};

struct PipelineDesc
{
	std::string name;
	std::string vertexSource;
	std::string fragmentSource;
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

	// Issue a draw against the currently-enabled pipeline. Callers are
	// expected to have called Enable() and any BindUniformBuffer() first.
	// `firstVertex`/`vertexCount` follow glDrawArrays semantics.
	//
	// Default is a no-op so legacy engine shader program objects that live
	// through IProgramObject do not need to implement it. Backend standalone
	// pipelines (GL + Metal) override this.
	virtual void Draw(PrimitiveTopology /*topology*/, uint32_t /*firstVertex*/, uint32_t /*vertexCount*/) {}
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
