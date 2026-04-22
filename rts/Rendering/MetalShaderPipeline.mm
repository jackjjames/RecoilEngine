#include "Rendering/Shaders/IShaderPipeline.h"

#include <memory>

namespace {

class MetalShaderPipeline final : public IShaderPipeline
{
public:
	explicit MetalShaderPipeline(PipelineDesc desc_)
		: desc(std::move(desc_))
	{}

	void BindAttribLocation(const std::string&, uint32_t) override {}
	void BindOutputLocation(const std::string&, uint32_t) override {}
	void Enable() override {}
	void Disable() override {}
	void Link() override { valid = true; }
	bool Validate() override { return valid; }
	void Release() override { valid = false; }
	void Reload(bool, bool validate) override
	{
		Link();
		if (validate)
			Validate();
	}
	void AttachShaderObject(Shader::IShaderObject*) override {}
	const std::string& GetName() const override { return desc.name; }
	const std::string& GetLog() const override { return log; }
	bool IsValid() const override { return valid; }

private:
	PipelineDesc desc;
	std::string log;
	bool valid = false;
};

} // namespace

std::unique_ptr<IShaderPipeline> CreateMetalShaderPipeline(const PipelineDesc& desc)
{
	auto pipeline = std::make_unique<MetalShaderPipeline>(desc);
	pipeline->Link();
	pipeline->Validate();
	return pipeline;
}
