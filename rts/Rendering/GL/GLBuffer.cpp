#include "Rendering/GL/GLBuffer.h"

#include "Rendering/IBuffer.h"
#include "Rendering/GL/myGL.h"

namespace {

class GLBuffer final : public IBuffer
{
public:
	GLBuffer(size_t size, const void* data)
		: sizeBytes(size)
	{
		glGenBuffers(1, &bufferId);
		glBindBuffer(GL_UNIFORM_BUFFER, bufferId);
		glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(sizeBytes), data, GL_DYNAMIC_DRAW);
		glBindBuffer(GL_UNIFORM_BUFFER, 0);
	}

	~GLBuffer() override
	{
		if (bufferId != 0)
			glDeleteBuffers(1, &bufferId);
	}

	bool IsValid() const override { return bufferId != 0; }
	uint32_t GetNativeId() const override { return bufferId; }
	size_t GetSize() const override { return sizeBytes; }

	void UpdateData(const void* data, size_t size, size_t offset) override
	{
		glBindBuffer(GL_UNIFORM_BUFFER, bufferId);
		glBufferSubData(GL_UNIFORM_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), data);
		glBindBuffer(GL_UNIFORM_BUFFER, 0);
	}

	void BindUniformRange(uint32_t slot, size_t offset, size_t size) const override
	{
		glBindBufferRange(GL_UNIFORM_BUFFER, slot, bufferId, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size));
	}

	void BindStorageRange(uint32_t slot, size_t offset, size_t size) const override
	{
		glBindBufferRange(GL_SHADER_STORAGE_BUFFER, slot, bufferId, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size));
	}

	void BindIndirect() const override
	{
		glBindBuffer(GL_DRAW_INDIRECT_BUFFER, bufferId);
	}

private:
	uint32_t bufferId = 0;
	size_t sizeBytes = 0;
};

} // namespace

std::unique_ptr<IBuffer> CreateGLBuffer(size_t size, const void* data)
{
	return std::make_unique<GLBuffer>(size, data);
}
