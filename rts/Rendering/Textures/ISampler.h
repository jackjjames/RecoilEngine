#pragma once

#include "Rendering/Textures/TextureCreationParams.hpp"

class ISampler
{
public:
	virtual ~ISampler() = default;

	virtual void Apply(uint32_t texTarget) const = 0;
	virtual const GL::TextureCreationParams& GetParams() const = 0;
};
