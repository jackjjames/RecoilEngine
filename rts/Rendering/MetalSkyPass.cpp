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

void main() {
    // Unproject the NDC coord into a world-space view ray. Vulkan NDC
    // has y-down; spirv-cross flips this transparently for MSL. Far
    // plane at z=1.
    vec4 farH  = ubo.uInvViewProj * vec4(vNDC, 1.0, 1.0);
    vec4 nearH = ubo.uInvViewProj * vec4(vNDC, 0.0, 1.0);
    vec3 farWS  = farH.xyz  / max(farH.w,  1e-6);
    vec3 nearWS = nearH.xyz / max(nearH.w, 1e-6);
    vec3 dir = normalize(farWS - nearWS);

    // Horizon-to-zenith blend via dir.y (view-space up). Use smoothstep
    // so the gradient is smooth even for cameras tilted above / below.
    float t = clamp(0.5 + 0.5 * dir.y, 0.0, 1.0);
    vec3 skyCol = mix(ubo.uHorizonColor.rgb, ubo.uZenithColor.rgb, smoothstep(0.0, 0.8, t));

    // Soft haze band near the horizon so the terrain has somewhere to
    // meet the sky without a hard line.
    float haze = smoothstep(0.1, 0.0, dir.y);
    skyCol = mix(skyCol, ubo.uHorizonColor.rgb * 1.1, haze * 0.65);

    // Sun disc + wide halo. Clamp to positive dot so the anti-sun
    // hemisphere is unaffected.
    vec3 L = normalize(ubo.uSunDirAndIntensity.xyz);
    float cosAng = max(dot(dir, L), 0.0);
    float sunDisc = smoothstep(0.9985, 0.9998, cosAng);
    float sunHalo = pow(cosAng, 64.0) * 0.35;
    skyCol += ubo.uSunColor.rgb * (sunDisc + sunHalo);

    fragColor = vec4(skyCol, 1.0);
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
	const auto& skyPtr = ISky::GetSky();
	if (skyPtr != nullptr && skyPtr->GetLight() != nullptr)
		sun = skyPtr->GetLight()->GetLightDir();
	ubo.sunDirAndIntensity[0] = sun.x;
	ubo.sunDirAndIntensity[1] = sun.y;
	ubo.sunDirAndIntensity[2] = sun.z;
	ubo.sunDirAndIntensity[3] = sun.w;

	ubo.horizonColor[0] = 0.62f; ubo.horizonColor[1] = 0.72f; ubo.horizonColor[2] = 0.80f; ubo.horizonColor[3] = 1.0f;
	ubo.zenithColor[0]  = 0.22f; ubo.zenithColor[1]  = 0.38f; ubo.zenithColor[2]  = 0.68f; ubo.zenithColor[3]  = 1.0f;
	ubo.sunColor[0]     = 1.00f; ubo.sunColor[1]     = 0.92f; ubo.sunColor[2]     = 0.78f; ubo.sunColor[3]     = 1.0f;

	uniformBuffer->UpdateData(&ubo, sizeof(ubo), 0);

	pipeline->Enable();
	pipeline->BindUniformBuffer(0, *uniformBuffer, 0, sizeof(ubo));
	pipeline->Draw(PrimitiveTopology::Triangles, 0, 3);
	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
