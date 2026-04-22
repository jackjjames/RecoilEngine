#include "Rendering/IBuffer.h"

#include <memory>

namespace {

class MetalBuffer final : public IBuffer
{
public:
	explicit MetalBuffer(size_t size_)
		: sizeBytes(size_)
	{}

	bool IsValid() const override { return true; }
	uint32_t GetNativeId() const override { return 0; }
	size_t GetSize() const override { return sizeBytes; }
	void UpdateData(const void*, size_t, size_t) override {}
	void BindUniformRange(uint32_t, size_t, size_t) const override {}

private:
	size_t sizeBytes = 0;
};

} // namespace

std::unique_ptr<IBuffer> CreateMetalBuffer(size_t size, const void*)
{
	return std::make_unique<MetalBuffer>(size);
}
