/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalWaterPlane.h"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/GlobalUnsynced.h"
#include "Map/MapDimensions.h"
#include "Map/ReadMap.h"
#include "Rendering/Env/ISky.h"
#include "Rendering/Env/SkyLight.h"
#include "Rendering/Env/WaterRendering.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Sim/Misc/GlobalConstants.h"
#include "System/Log/ILog.h"
#include "System/float3.h"
#include "System/float4.h"

#include <cstring>


namespace {

struct WaterVertex { float x, z; };

struct alignas(16) UBOLayout {
	float viewProj[16];
	float camPos[4];          // xyz, w = time
	float sunDir[4];          // xyz, w unused
	float surfaceColor[4];    // rgb, w = surfaceAlpha
	float minColor[4];        // rgb, w = unused (deep-water tint)
	float horizonColor[4];    // rgb, w = unused (sky reflection palette)
	float sunColor[4];        // rgb, w = unused (specular tint)
	float wavePhase[4];        // wind speed, wave length, foam intensity, fresnel power
};

constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec2 aPosXZ;
layout(location = 0) out vec3 vWorldPos;

layout(set = 0, binding = 0) uniform WaterUBO {
    mat4 uViewProj;
    vec4 uCamPos;
    vec4 uSunDir;
    vec4 uSurfaceColor;
    vec4 uMinColor;
    vec4 uHorizonColor;
    vec4 uSunColor;
    vec4 uWavePhase;
} ubo;

void main() {
    // Water sits at world y = 0 by convention in Spring/BAR (the
    // engine's "sea level"). The plane is a single quad sized to the
    // map; the fragment shader does the heavy lifting.
    vec3 world = vec3(aPosXZ.x, 0.0, aPosXZ.y);
    vWorldPos = world;
    gl_Position = ubo.uViewProj * vec4(world, 1.0);
}
)";

constexpr const char* kFragmentGlsl = R"(#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform WaterUBO {
    mat4 uViewProj;
    vec4 uCamPos;          // xyz, w = time (seconds, mod-game-time)
    vec4 uSunDir;
    vec4 uSurfaceColor;    // map "WaterSurfaceColor" + surfaceAlpha
    vec4 uMinColor;        // deep-water tint
    vec4 uHorizonColor;    // sky horizon for fresnel reflection
    vec4 uSunColor;        // specular highlight tint
    vec4 uWavePhase;       // x = windSpeed, y = waveLength, z = foam, w = fresnelPow
} ubo;

// Cheap analytic ripple normal: stack a few moving sine waves with
// orthogonal directions so the surface picks up directional motion
// without the cost of a real Gerstner/FFT. World XZ is used so the
// pattern stays anchored to map space, not to viewport.
vec3 RippleNormal(vec2 p, float t) {
    float w = max(ubo.uWavePhase.y, 1.0);
    vec2 d1 = vec2( 0.86,  0.50);
    vec2 d2 = vec2(-0.30,  0.95);
    vec2 d3 = vec2( 0.51, -0.86);
    float phase1 = dot(p, d1) / w + t * (0.45 + ubo.uWavePhase.x * 0.05);
    float phase2 = dot(p, d2) / (w * 0.6) + t * (0.65 + ubo.uWavePhase.x * 0.07);
    float phase3 = dot(p, d3) / (w * 1.7) + t * (0.30 + ubo.uWavePhase.x * 0.03);

    float h1 = sin(phase1);
    float h2 = sin(phase2);
    float h3 = sin(phase3);

    // dH/dx, dH/dz from analytic derivatives. Amplitudes biased so the
    // mid-frequency wave dominates while the others add chop.
    float dHx = 0.045 * cos(phase1) * d1.x / w
              + 0.030 * cos(phase2) * d2.x / (w * 0.6)
              + 0.020 * cos(phase3) * d3.x / (w * 1.7);
    float dHz = 0.045 * cos(phase1) * d1.y / w
              + 0.030 * cos(phase2) * d2.y / (w * 0.6)
              + 0.020 * cos(phase3) * d3.y / (w * 1.7);

    return normalize(vec3(-dHx, 1.0, -dHz));
}

void main() {
    float t = ubo.uCamPos.w;
    vec3 N = RippleNormal(vWorldPos.xz, t);
    vec3 V = normalize(ubo.uCamPos.xyz - vWorldPos);
    vec3 L = normalize(ubo.uSunDir.xyz);
    vec3 R = reflect(-V, N);

    // Schlick fresnel: 0 at normal incidence, 1 at glancing. The
    // exponent comes from waterRendering->fresnelPower so map-defined
    // "FresnelPower" still drives the look.
    float fresnel = pow(1.0 - max(dot(N, V), 0.0),
                        max(ubo.uWavePhase.w, 1.5));
    fresnel = clamp(fresnel, 0.05, 0.95);

    // Reflection: the analytic skydome (rough blend of horizon and a
    // fake zenith rim toward the sky's "up"). Cheap stand-in for the
    // real RTT reflection that lands with the IRenderTarget colour-
    // attachment slice.
    float skyT = clamp(R.y, 0.0, 1.0);
    vec3 skyRefl = mix(ubo.uHorizonColor.rgb,
                       ubo.uHorizonColor.rgb * 1.35 + vec3(0.06, 0.10, 0.18),
                       skyT);

    // Refraction surrogate: blend surface to deep tint based on view
    // angle. At grazing angles we mostly see surface colour; looking
    // straight down the deep-water minColor takes over.
    float depthLook = clamp(dot(N, V), 0.0, 1.0);
    vec3 refrCol = mix(ubo.uSurfaceColor.rgb, ubo.uMinColor.rgb,
                       1.0 - depthLook);

    vec3 col = mix(refrCol, skyRefl, fresnel);

    // Sun specular highlight. Squeezed exponent gives the BAR glint
    // shape without a normal-mapped surface.
    float specPow = 64.0;
    float spec = pow(max(dot(R, L), 0.0), specPow);
    col += ubo.uSunColor.rgb * spec * (0.9 + 0.5 * fresnel);

    // Foam / sparkle: a high-frequency mask along the wave crests.
    float foamMask = smoothstep(0.985, 1.0,
                                sin(vWorldPos.x * 0.07 + t * 1.7) *
                                sin(vWorldPos.z * 0.05 - t * 1.3));
    col += vec3(1.0) * foamMask * ubo.uWavePhase.z * 0.4;

    // Per-fragment alpha: opacity scales with how steeply we're
    // looking down (top-down sees through, grazing reads as solid).
    float alpha = clamp(ubo.uSurfaceColor.a + (1.0 - depthLook) * 0.25,
                        0.0, 1.0);

    fragColor = vec4(col, alpha);
}
)";

} // namespace


MetalWaterPlane::MetalWaterPlane()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;
	auto& backend = *globalRendering->renderBackend;

	UBOLayout seed{};
	seed.viewProj[0]  = 1.0f; seed.viewProj[5]  = 1.0f;
	seed.viewProj[10] = 1.0f; seed.viewProj[15] = 1.0f;
	uniformBuffer = backend.CreateBuffer(sizeof(UBOLayout), &seed);
	if (!uniformBuffer || !uniformBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalWaterPlane] uniform buffer creation failed");
		return;
	}

	PipelineDesc pd;
	pd.name = "water_plane";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	pd.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = offsetof(WaterVertex, x), .format = VertexFormat::Float2 },
	};
	pd.vertexBindings = {
		VertexBindingLayout{ .slot = 0, .stride = sizeof(WaterVertex) },
	};
	// Standard alpha blend. The surface alpha includes both the
	// map's "WaterSurfaceAlpha" parameter and a view-angle factor
	// from the fragment so partially submerged terrain still reads
	// through at top-down framing.
	pd.blendState.enabled  = true;
	pd.blendState.srcColor = GL_SRC_ALPHA;
	pd.blendState.dstColor = GL_ONE_MINUS_SRC_ALPHA;
	pd.blendState.srcAlpha = GL_SRC_ALPHA;
	pd.blendState.dstAlpha = GL_ONE_MINUS_SRC_ALPHA;

	pipeline = backend.CreatePipeline(pd);
	if (!pipeline || !pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalWaterPlane] pipeline link failed: %s",
			pipeline ? pipeline->GetLog().c_str() : "(null)");
		return;
	}

	valid = true;
}

MetalWaterPlane::~MetalWaterPlane() = default;

void MetalWaterPlane::Draw() const
{
	if (!valid || readMap == nullptr)
		return;

	// "Don't bother rendering water on land-only maps" guard. Mirrors
	// the GL build's IWater::ForceRendering behaviour: maps without a
	// declared underwater bound and without an explicit forceRendering
	// flag (e.g. desert / mesa maps) keep the background dry. Any map
	// with currMinHeight < 0 has actual underwater geometry and gets
	// the surface drawn unconditionally.
	const bool forceWater = waterRendering->forceRendering;
	if (!forceWater && readMap->GetCurrMinHeight() >= 0.0f)
		return;

	const CCamera* cam = CCameraHandler::GetActiveCamera();
	if (cam == nullptr)
		return;

	// Camera below sea level: we'd be drawing the underside of the
	// quad on top of itself, which inverts the fresnel and produces
	// upside-down sky reflections. Skip until a proper underwater
	// pass lands (the GL build has a separate underwater fog path);
	// the unit and terrain passes still read correctly above water.
	if (cam->GetPos().y <= 0.0f)
		return;

	const float mapW = static_cast<float>(mapDims.mapx) * static_cast<float>(SQUARE_SIZE);
	const float mapH = static_cast<float>(mapDims.mapy) * static_cast<float>(SQUARE_SIZE);

	// Single-quad geometry built on demand. The mesh is uploaded each
	// draw because it's tiny (4 verts, 6 indices) - cheaper than
	// keeping persistent buffers warm and dealing with the static
	// re-init dance for the rare case of map size changing mid-game.
	const WaterVertex verts[4] = {
		{ 0.0f, 0.0f },
		{ mapW, 0.0f },
		{ mapW, mapH },
		{ 0.0f, mapH },
	};
	const uint32_t inds[6] = { 0, 1, 2, 0, 2, 3 };

	auto& backend = *globalRendering->renderBackend;
	auto vb = backend.CreateBuffer(sizeof(verts), verts);
	auto ib = backend.CreateBuffer(sizeof(inds),  inds);
	if (!vb || !ib || !vb->IsValid() || !ib->IsValid())
		return;

	UBOLayout ubo{};
	std::memcpy(ubo.viewProj, cam->GetViewProjectionMatrix().m, sizeof(ubo.viewProj));

	const float3& p = cam->GetPos();
	ubo.camPos[0] = p.x; ubo.camPos[1] = p.y; ubo.camPos[2] = p.z;
	ubo.camPos[3] = (gu != nullptr) ? gu->modGameTime : 0.0f;

	float4 sun(0.3f, 0.85f, 0.4f, 1.0f);
	float3 mapSkyCol(0.22f, 0.38f, 0.68f);
	float3 mapSunCol(1.00f, 0.92f, 0.78f);
	const auto& skyPtr = ISky::GetSky();
	if (skyPtr != nullptr) {
		if (skyPtr->GetLight() != nullptr)
			sun = skyPtr->GetLight()->GetLightDir();
		mapSkyCol = skyPtr->skyColor;
		mapSunCol = skyPtr->sunColor;
	}
	ubo.sunDir[0] = sun.x; ubo.sunDir[1] = sun.y; ubo.sunDir[2] = sun.z;

	// Surface and deep colours come straight from the map's water
	// stanza (waterRendering globals). Maps that don't ship a stanza
	// keep CWaterRendering's defaults, which look like generic blue
	// water - good enough until the proper BumpWater port lands.
	ubo.surfaceColor[0] = waterRendering->surfaceColor.x;
	ubo.surfaceColor[1] = waterRendering->surfaceColor.y;
	ubo.surfaceColor[2] = waterRendering->surfaceColor.z;
	ubo.surfaceColor[3] = waterRendering->surfaceAlpha;

	ubo.minColor[0]     = waterRendering->minColor.x;
	ubo.minColor[1]     = waterRendering->minColor.y;
	ubo.minColor[2]     = waterRendering->minColor.z;

	// Same horizon synthesis MetalSkyPass uses, so the water's
	// fresnel reflection picks up the actual sky we're drawing.
	const float3 horizon = mapSkyCol * 0.35f + mapSunCol * 0.55f
	                     + float3(0.05f, 0.05f, 0.04f);
	ubo.horizonColor[0] = horizon.x;
	ubo.horizonColor[1] = horizon.y;
	ubo.horizonColor[2] = horizon.z;

	ubo.sunColor[0] = mapSunCol.x;
	ubo.sunColor[1] = mapSunCol.y;
	ubo.sunColor[2] = mapSunCol.z;

	ubo.wavePhase[0] = waterRendering->windSpeed;
	ubo.wavePhase[1] = waterRendering->waveLength;
	ubo.wavePhase[2] = waterRendering->waveFoamIntensity;
	ubo.wavePhase[3] = waterRendering->fresnelPower;

	uniformBuffer->UpdateData(&ubo, sizeof(ubo), 0);

	pipeline->Enable();
	pipeline->BindUniformBuffer(0, *uniformBuffer, 0, sizeof(ubo));
	pipeline->BindVertexBuffer(0, *vb);
	pipeline->DrawIndexed(PrimitiveTopology::Triangles, 6, IndexType::Uint32, *ib);
	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
