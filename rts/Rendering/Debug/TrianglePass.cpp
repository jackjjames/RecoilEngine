#include "Rendering/Debug/TrianglePass.h"

#include "Rendering/GlobalRendering.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "System/Config/ConfigHandler.h"
#include "System/float4.h"

#include "Rendering/GL/myGL.h"

CONFIG(bool, DebugTrianglePass).defaultValue(false).description("Renders a backend-seam validation triangle before the world pass.");

namespace {

struct alignas(16) TrianglePassUniforms
{
	float4 color = float4(1.0f, 1.0f, 1.0f, 1.0f);
};

PipelineDesc BuildTrianglePipelineDesc()
{
	return {
		.name = "TrianglePass",
		.vertexSource = R"(
#version 330 core
const vec2 positions[3] = vec2[3](
	vec2(-0.75, -0.75),
	vec2( 0.75, -0.75),
	vec2( 0.00,  0.75)
);
void main() {
	gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
)",
		.fragmentSource = R"(
#version 330 core
layout(std140, binding = 0) uniform TrianglePassData {
	vec4 color;
};
out vec4 outColor;
void main() {
	outColor = color;
}
)",
	};
}

} // namespace

TrianglePass trianglePass;

void TrianglePass::Draw()
{
	if (!configHandler->GetBool("DebugTrianglePass"))
		return;
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;

	static auto pipeline = globalRendering->renderBackend->CreatePipeline(BuildTrianglePipelineDesc());
	static auto buffer = globalRendering->renderBackend->CreateBuffer(sizeof(TrianglePassUniforms), nullptr);
	static TrianglePassUniforms uniforms;

	if (pipeline == nullptr || !pipeline->IsValid())
		return;
	if (buffer == nullptr || !buffer->IsValid())
		return;

	buffer->UpdateData(&uniforms, sizeof(uniforms), 0);
	pipeline->BindUniformBuffer(0, *buffer, 0, sizeof(uniforms));
	pipeline->Enable();

	if (dynamic_cast<GLShaderPipeline*>(pipeline.get()) != nullptr)
		glDrawArrays(GL_TRIANGLES, 0, 3);

	pipeline->Disable();
}
