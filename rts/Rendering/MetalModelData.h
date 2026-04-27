/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

class CFeature;
class CUnit;
class ScopedTransformMemAlloc;

namespace MetalModelData
{
	void Init();
	void Kill();
	void Update();

	const ScopedTransformMemAlloc& GetTransformMemAlloc(const CUnit* unit);
	const ScopedTransformMemAlloc& GetTransformMemAlloc(const CFeature* feature);
}

#endif // RENDER_BACKEND_METAL
