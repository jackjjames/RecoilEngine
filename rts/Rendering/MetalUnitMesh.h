/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <memory>
#include <unordered_map>

class IBuffer;
class IShaderPipeline;
struct S3DModel;

// Minimum-viable 3D unit drawer. For every active unit whose model has
// geometry attached, draws the model as a flat-shaded grey mesh in the
// unit's bind pose at the unit's world transform. Lazily uploads one
// set of vertex / index buffers per unique S3DModel pointer (commander
// spawns for every team share the same model so this is the granularity
// that matters in practice).
//
// Intentionally missing for this slice (S9-C4a):
//   - per-piece animation; the LocalModel tree is ignored and pieces are
//     drawn in bind pose only.
//   - team colouring; output is pure grey-lit.
//   - texturing; we use a constant material in the fragment shader.
//   - LOD + frustum culling; every active unit is submitted every frame.
//   - shadows / deferred path.
//
// S9-C4b picks up team colour + simple texture sampling from the S3O
// atlas; full animation / per-piece transforms + transformsUploader SSBO
// parity lands after the projectile + feature drawers (they also want
// that path).
class MetalUnitMesh
{
public:
	MetalUnitMesh();
	~MetalUnitMesh();

	MetalUnitMesh(const MetalUnitMesh&) = delete;
	MetalUnitMesh& operator=(const MetalUnitMesh&) = delete;

	bool IsValid() const { return valid; }

	void Draw();

private:
	struct ModelBuffers {
		std::unique_ptr<IBuffer> vertexBuffer;
		std::unique_ptr<IBuffer> indexBuffer;
		uint32_t indexCount = 0;
	};

	// Upload the flattened bind-pose mesh for `model` into GPU buffers
	// and cache the result. Returns null when the model has no drawable
	// geometry (e.g. aircraft wrecks that were stripped at load time).
	const ModelBuffers* GetOrUploadModel(const S3DModel* model);

	std::unique_ptr<IShaderPipeline>            pipeline;
	std::unique_ptr<IBuffer>                    uniformBuffer;
	std::unordered_map<const S3DModel*, ModelBuffers> modelCache;

	bool valid = false;
};

#endif // RENDER_BACKEND_METAL
