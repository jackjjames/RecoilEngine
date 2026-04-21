/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cstdint>
#include <string>

namespace Shader {
	struct IShaderObject;
}

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
	virtual unsigned int GetObjID() const = 0;
};
