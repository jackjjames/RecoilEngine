/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <cstdint>
#include <memory>

class IBuffer;
class IShaderPipeline;

// Cheap analytic projectile tracer renderer. Walks the synced and
// unsynced projectile lists, projects each one into a billboarded
// streak that is stretched along the projectile's velocity vector,
// and emits an additive-blended soft-edged glow tinted by team
// colour. No texture atlas, no per-projectile WeaponDef visuals;
// the proper CProjectileDrawer port (later S9 slice) replaces this
// once the WeaponDef -> ProjectileDrawer mapping comes online.
//
// Drawn after units so tracers read on top of the silhouette
// instead of being overwritten. Additive blend so overlapping
// tracers brighten naturally.
class MetalProjectiles
{
public:
	MetalProjectiles();
	~MetalProjectiles();

	MetalProjectiles(const MetalProjectiles&) = delete;
	MetalProjectiles& operator=(const MetalProjectiles&) = delete;

	bool IsValid() const { return valid; }

	void Draw();

private:
	std::unique_ptr<IShaderPipeline> pipeline;
	std::unique_ptr<IBuffer>         vertexBuffer;
	std::unique_ptr<IBuffer>         indexBuffer;
	std::unique_ptr<IBuffer>         uniformBuffer;

	uint32_t bufferCapacity = 0;

	bool valid = false;
};

#endif // RENDER_BACKEND_METAL
