#pragma once

#include <memory>

class IShaderPipeline;
struct PipelineDesc;

std::unique_ptr<IShaderPipeline> CreateGLStandaloneShaderPipeline(const PipelineDesc& desc);
