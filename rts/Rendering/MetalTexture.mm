/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Placeholder for the Metal ITexture implementation. Today only exposes
// nullptr stubs for the MetalResources accessors so the shader pipeline can
// compile against them; the actual MTLTexture / MTLSamplerState creation +
// upload path lands with S8-C5b. Kept in a dedicated .mm so that work can
// replace the body without touching headers.

#include "Rendering/MetalResources.h"

namespace MetalResources {

void* GetMtlTexture(const ITexture& /*texture*/)
{
	return nullptr;
}

void* GetMtlSampler(const ITexture& /*texture*/)
{
	return nullptr;
}

} // namespace MetalResources
