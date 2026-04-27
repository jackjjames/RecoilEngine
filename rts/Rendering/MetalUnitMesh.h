/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <memory>
#include <unordered_map>
#include <vector>

class IBuffer;
class IShaderPipeline;
class ITexture;
struct S3DModel;

// 3D unit + feature drawer. For every active unit / feature whose model
// has geometry attached, walks the LocalModel piece tree and draws each
// piece at its current animated transform (unit world transform * piece
// model-space transform). S3O tex1/tex2 sampling follows GL's
// springcontent ModelFragProg.glsl convention: tex1 alpha drives
// team-colour replacement, tex2 R is self-illumination, tex2 G drives
// specular/reflectivity, and tex2 A contributes opacity.
// Lazily uploads one vertex / index buffer pair per S3DModelPiece.
//
// Intentionally missing:
//   - real cubemap/environment reflections.
//   - LOD + frustum culling; every active object is submitted every frame.
//   - shadow pass (S9-C5a).
//   - GPU-side transform SSBO: matrices are pushed through the per-draw
//     UBO. Per-piece draw call counts top out around 3-4k for medium
//     skirmishes which is comfortable on Metal; the SSBO path lands when
//     instancing becomes the bottleneck.
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
	struct PieceBuffers {
		std::unique_ptr<IBuffer> vertexBuffer;
		std::unique_ptr<IBuffer> indexBuffer;
		uint32_t indexCount = 0;
	};
	struct ModelBuffers {
		// pieces[i] aligns with S3DModel::pieceObjects[i] /
		// LocalModel::pieces[i]; empty slots (piece without geometry,
		// or upload failure) carry a default-constructed PieceBuffers
		// so the lookup stays branch-free.
		std::vector<PieceBuffers> pieces;
		bool anyGeometry = false;
	};

	// Upload one vertex / index buffer pair per geometry-bearing piece
	// of `model`. Returns null when the model has no drawable geometry.
	const ModelBuffers* GetOrUploadModel(const S3DModel* model);

	std::unique_ptr<IShaderPipeline>            pipeline;
	std::unique_ptr<IBuffer>                    uniformBuffer;
	std::unique_ptr<ITexture>                   whiteTexture;
	std::unique_ptr<ITexture>                   blackTexture;
	std::unordered_map<const S3DModel*, ModelBuffers> modelCache;

	bool valid = false;
};

#endif // RENDER_BACKEND_METAL
