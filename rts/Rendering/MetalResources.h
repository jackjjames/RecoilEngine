/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

// Tiny accessor layer that lets Metal-only code paths fish the raw
// id<MTLBuffer> / id<MTLTexture> / id<MTLSamplerState> handles out of their
// IBuffer / ITexture wrappers without exposing the concrete classes across
// headers. The functions return `void*` pointing to the retained Objective-C
// object so callers can __bridge back as needed. All return nullptr if the
// resource isn't actually Metal-backed.
//
// Only the Metal backend implements these; the GL build must not link this
// header (guard with RENDER_BACKEND_METAL).

class IBuffer;
class ITexture;

namespace MetalResources {

void* GetMtlBuffer(const IBuffer& buffer);

void* GetMtlTexture(const ITexture& texture);
void* GetMtlSampler(const ITexture& texture);

} // namespace MetalResources
