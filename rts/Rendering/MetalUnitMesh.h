/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <memory>
#include <unordered_map>

class IBuffer;
class IShaderPipeline;
class ITexture;
struct S3DModel;

// 3D unit + feature drawer. For every active unit / feature whose model
// has geometry attached, draws the model in the bind pose at the unit's
// world transform with diffuse texture sampling and alpha-mask team
// colour replacement (matches GL's springcontent ModelFragProg.glsl
// convention: alpha=0 keeps diffuse, alpha=1 swaps to team colour).
// Lazily uploads one vertex / index buffer pair per S3DModel pointer.
//
// Intentionally missing:
//   - per-piece animation; the LocalModel tree is ignored and pieces are
//     drawn in bind pose only. Lands with TransformsUploader + an SSBO of
//     piece matrices (S9-C4b part 2).
//   - tex2 / specular / self-illumination sampling.
//   - LOD + frustum culling; every active object is submitted every frame.
//   - shadow pass (S9-C5a).
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
	std::unique_ptr<ITexture>                   whiteTexture;
	std::unordered_map<const S3DModel*, ModelBuffers> modelCache;

	bool valid = false;
};

#endif // RENDER_BACKEND_METAL
