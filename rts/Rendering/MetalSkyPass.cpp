/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalSkyPass.h"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Rendering/Env/ISky.h"
#include "Rendering/Env/SkyLight.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "System/Log/ILog.h"
#include "System/float4.h"

#include <cstring>


namespace {

// Sky parameters fed to the fragment shader. Aspect + inv-viewProj let
// the fragment reconstruct a world-space view ray for each pixel; sun
// direction drives the sun disc + haze tinting.
struct alignas(16) SkyUBOLayout {
	float invViewProj[16];
	float camPos[4];               // xyz, unused
	float sunDirAndIntensity[4];   // xyz, intensity (sky.w)
	float horizonColor[4];
	float zenithColor[4];
	float sunColor[4];
};

constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) out vec2 vNDC;

// Synthesise a full-screen triangle without a vertex buffer. CCW when
// viewed from the front; y-flipped UV lets the fragment stage reason
// about the viewport in NDC directly.
void main() {
    vec2 positions[3] = vec2[3](
        vec2(-1.0, -3.0),
        vec2(-1.0,  1.0),
        vec2( 3.0,  1.0)
    );
    vec2 p = positions[gl_VertexIndex];
    vNDC = p;
    gl_Position = vec4(p, 1.0, 1.0); // depth=1 so terrain at lower depths overdraws
}
)";

constexpr const char* kFragmentGlsl = R"(#version 450
layout(location = 0) in vec2 vNDC;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform SkyUBO {
    mat4 uInvViewProj;
    vec4 uCamPos;
    vec4 uSunDirAndIntensity;
    vec4 uHorizonColor;
    vec4 uZenithColor;
    vec4 uSunColor;
} ubo;

// Cheap analytic sky shading. Approximates Rayleigh + Mie scattering
// without doing the full integral - just enough so the horizon picks
// up warm sun-side tinting and the zenith reads as a deeper blue.
// The map-driven uHorizonColor / uZenithColor / uSunColor act as the
// "tinted base palette" the scattering modulates around, so per-map
// atmosphere stanzas (BAR's "atmosphere { skyColor = ... }") still
// drive the look instead of being overridden by hard-coded values.

vec3 RayleighTint(float cosTheta) {
    // Simple wavelength-dependent falloff: blue scatters most, red
    // least. cosTheta = dot(dir, sun); using (1 + cos^2) keeps the
    // tint smooth across the dome, peaking opposite the sun.
    return vec3(0.55, 0.78, 1.00) * (0.55 + 0.45 * (1.0 - cosTheta * cosTheta));
}

float MiePhase(float cosTheta, float g) {
    // Henyey-Greenstein phase function. g near 1 = forward-scattered
    // halo around the sun; we use a moderate value so the halo is
    // present but doesn't wash out the whole sky.
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(denom, 1e-4), 1.5));
}

void main() {
    // Unproject NDC into a world-space view ray. Far plane at z=1;
    // SPIRV-Cross flips Vulkan NDC into Metal's y-down clip space
    // transparently so vNDC reads as expected.
    vec4 farH  = ubo.uInvViewProj * vec4(vNDC, 1.0, 1.0);
    vec4 nearH = ubo.uInvViewProj * vec4(vNDC, 0.0, 1.0);
    vec3 farWS  = farH.xyz  / max(farH.w,  1e-6);
    vec3 nearWS = nearH.xyz / max(nearH.w, 1e-6);
    vec3 dir = normalize(farWS - nearWS);

    vec3 L = normalize(ubo.uSunDirAndIntensity.xyz);
    float sunUp = clamp(L.y, 0.0, 1.0);
    float cosTheta = clamp(dot(dir, L), -1.0, 1.0);
    float upDot = clamp(dir.y, -0.2, 1.0);

    // Vertical gradient: zenith above, horizon at the equator. Bias
    // the smoothstep so the bottom 20% of the dome is nearly pure
    // horizon colour - matches how BAR maps render the world meeting
    // the haze band.
    float zenithT = smoothstep(0.0, 0.85, upDot);
    vec3 base = mix(ubo.uHorizonColor.rgb, ubo.uZenithColor.rgb, zenithT);

    // Rayleigh tint on the base, modulated by sun elevation: noon
    // sun = strong blue scatter, sunset sun = warm red horizon.
    vec3 rayleighCol = base * RayleighTint(cosTheta);
    vec3 sunsetTint  = mix(vec3(1.0, 0.55, 0.30), vec3(1.0), sunUp);
    rayleighCol *= sunsetTint;

    // Mie sun halo. Soft wide glow + a hot disc on top. Halo intensity
    // scales with sun elevation so the glow doesn't blow out at noon.
    float mie = MiePhase(cosTheta, 0.78) * (0.6 + 0.4 * sunUp);
    float sunDisc = smoothstep(0.9985, 0.9998, cosTheta);
    vec3 sunCol = ubo.uSunColor.rgb;

    // Horizon haze: ground meets sky through a soft warm band whose
    // height tracks the inverse of sun elevation (low sun = thicker
    // haze). Multiplied against the base so dark zenith stays dark.
    float hazeBand = smoothstep(0.18, 0.0, dir.y);
    float hazeAmt  = hazeBand * (0.6 + 0.4 * (1.0 - sunUp));
    vec3 hazeCol = mix(rayleighCol, ubo.uHorizonColor.rgb * sunsetTint, 0.5);

    vec3 sky = mix(rayleighCol, hazeCol, hazeAmt);
    sky += sunCol * (mie * 0.18 + sunDisc * 1.5);

    // Anti-sun rim / opposition effect: subtly lift the hemisphere
    // opposite the sun so the dome has direction even when looking
    // backwards. Without this the back half feels flat.
    float backLight = smoothstep(0.0, 0.6, -cosTheta) * (1.0 - sunUp) * 0.08;
    sky += ubo.uHorizonColor.rgb * backLight;

    // Soft gamma/tone shape - keep highlights from blowing out around
    // the sun while preserving the bright disc itself.
    sky = sky / (sky + vec3(0.65));
    sky *= 1.65;

    fragColor = vec4(sky, 1.0);
}
)";

} // namespace


MetalSkyPass::MetalSkyPass()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;
	auto& backend = *globalRendering->renderBackend;

	SkyUBOLayout seed{};
	seed.invViewProj[0]  = 1.0f;
	seed.invViewProj[5]  = 1.0f;
	seed.invViewProj[10] = 1.0f;
	seed.invViewProj[15] = 1.0f;
	seed.sunDirAndIntensity[1] = 1.0f;
	seed.horizonColor[0] = 0.62f; seed.horizonColor[1] = 0.72f; seed.horizonColor[2] = 0.80f;
	seed.zenithColor[0]  = 0.22f; seed.zenithColor[1]  = 0.38f; seed.zenithColor[2]  = 0.68f;
	seed.sunColor[0]     = 1.00f; seed.sunColor[1]     = 0.92f; seed.sunColor[2]     = 0.78f;

	uniformBuffer = backend.CreateBuffer(sizeof(SkyUBOLayout), &seed);
	if (!uniformBuffer || !uniformBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalSkyPass] uniform buffer creation failed");
		return;
	}

	PipelineDesc pd;
	pd.name = "sky_gradient";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	// Empty vertex attributes + bindings: vertex-less pipeline. MetalShader
	// Pipeline leaves MTLRenderPipelineDescriptor.vertexDescriptor nil in
	// that case, which is exactly what procedural gl_VertexIndex paths need.

	pipeline = backend.CreatePipeline(pd);
	if (!pipeline || !pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalSkyPass] pipeline link failed: %s",
			pipeline ? pipeline->GetLog().c_str() : "(null)");
		return;
	}

	valid = true;
}

MetalSkyPass::~MetalSkyPass() = default;

void MetalSkyPass::Draw() const
{
	if (!valid)
		return;

	SkyUBOLayout ubo{};

	// Inverse view-projection for view-ray reconstruction in the fragment
	// stage; CCamera keeps a pre-inverted cache so we read it directly.
	const CCamera* cam = CCameraHandler::GetActiveCamera();
	if (cam != nullptr) {
		std::memcpy(ubo.invViewProj, cam->GetViewProjectionMatrixInverse().m, sizeof(ubo.invViewProj));
		const float3& p = cam->GetPos();
		ubo.camPos[0] = p.x; ubo.camPos[1] = p.y; ubo.camPos[2] = p.z;
	} else {
		ubo.invViewProj[0]  = 1.0f;
		ubo.invViewProj[5]  = 1.0f;
		ubo.invViewProj[10] = 1.0f;
		ubo.invViewProj[15] = 1.0f;
	}

	float4 sun = float4(0.3f, 0.85f, 0.4f, 1.0f);
	float3 mapSkyCol = float3(0.22f, 0.38f, 0.68f);
	float3 mapSunCol = float3(1.00f, 0.92f, 0.78f);
	const auto& skyPtr = ISky::GetSky();
	if (skyPtr != nullptr) {
		if (skyPtr->GetLight() != nullptr)
			sun = skyPtr->GetLight()->GetLightDir();
		mapSkyCol = skyPtr->skyColor;
		mapSunCol = skyPtr->sunColor;
	}
	ubo.sunDirAndIntensity[0] = sun.x;
	ubo.sunDirAndIntensity[1] = sun.y;
	ubo.sunDirAndIntensity[2] = sun.z;
	ubo.sunDirAndIntensity[3] = sun.w;

	// Horizon = warmed-up zenith colour. Maps only ship a single
	// "skyColor" through atmosphere{}, so we synthesise the gradient
	// by lifting the warm channels and pushing toward the sun colour
	// for the band that meets the ground.
	const float3 horizon = mapSkyCol * 0.35f + mapSunCol * 0.55f + float3(0.05f, 0.05f, 0.04f);
	ubo.horizonColor[0] = horizon.x; ubo.horizonColor[1] = horizon.y; ubo.horizonColor[2] = horizon.z; ubo.horizonColor[3] = 1.0f;
	ubo.zenithColor[0]  = mapSkyCol.x; ubo.zenithColor[1]  = mapSkyCol.y; ubo.zenithColor[2]  = mapSkyCol.z; ubo.zenithColor[3]  = 1.0f;
	ubo.sunColor[0]     = mapSunCol.x; ubo.sunColor[1]     = mapSunCol.y; ubo.sunColor[2]     = mapSunCol.z; ubo.sunColor[3]     = 1.0f;

	uniformBuffer->UpdateData(&ubo, sizeof(ubo), 0);

	pipeline->Enable();
	pipeline->BindUniformBuffer(0, *uniformBuffer, 0, sizeof(ubo));
	pipeline->Draw(PrimitiveTopology::Triangles, 0, 3);
	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
