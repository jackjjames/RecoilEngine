#include "Rendering/GL/GLStandaloneShaderPipeline.h"

#include "Rendering/Shaders/IShaderPipeline.h"
#include "Rendering/GL/myGL.h"

#include <memory>
#include <vector>

namespace {

GLuint CompileShader(GLenum type, const std::string& source, std::string& log)
{
	const GLuint shader = glCreateShader(type);
	const char* src = source.c_str();
	glShaderSource(shader, 1, &src, nullptr);
	glCompileShader(shader);

	GLint success = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &success);

	if (success == GL_FALSE) {
		GLint logSize = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logSize);
		log.resize(logSize > 0 ? logSize : 0);
		if (!log.empty())
			glGetShaderInfoLog(shader, logSize, nullptr, log.data());
		glDeleteShader(shader);
		return 0;
	}

	return shader;
}

class GLStandaloneShaderPipeline final : public GLShaderPipeline
{
public:
	explicit GLStandaloneShaderPipeline(const PipelineDesc& desc_)
		: desc(desc_)
	{}

	~GLStandaloneShaderPipeline() override
	{
		Release();
	}

	void BindAttribLocation(const std::string& name, uint32_t index) override
	{
		if (programId != 0)
			glBindAttribLocation(programId, index, name.c_str());
	}

	void BindOutputLocation(const std::string& name, uint32_t index) override
	{
		if (programId != 0)
			glBindFragDataLocation(programId, index, name.c_str());
	}

	void Enable() override
	{
		if (programId != 0)
			glUseProgram(programId);
	}

	void Disable() override
	{
		glUseProgram(0);
	}

	void Link() override
	{
		Release();
		log.clear();

		const GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, desc.vertexSource, log);
		if (vertexShader == 0) {
			valid = false;
			return;
		}

		const GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, desc.fragmentSource, log);
		if (fragmentShader == 0) {
			glDeleteShader(vertexShader);
			valid = false;
			return;
		}

		programId = glCreateProgram();
		glAttachShader(programId, vertexShader);
		glAttachShader(programId, fragmentShader);
		glLinkProgram(programId);

		GLint success = GL_FALSE;
		glGetProgramiv(programId, GL_LINK_STATUS, &success);

		if (success == GL_FALSE) {
			GLint logSize = 0;
			glGetProgramiv(programId, GL_INFO_LOG_LENGTH, &logSize);
			log.resize(logSize > 0 ? logSize : 0);
			if (!log.empty())
				glGetProgramInfoLog(programId, logSize, nullptr, log.data());
			glDeleteProgram(programId);
			programId = 0;
		}

		glDeleteShader(vertexShader);
		glDeleteShader(fragmentShader);
		valid = (programId != 0);
	}

	bool Validate() override
	{
		if (programId == 0)
			return false;

		glValidateProgram(programId);
		GLint success = GL_FALSE;
		glGetProgramiv(programId, GL_VALIDATE_STATUS, &success);
		valid = (success == GL_TRUE);
		return valid;
	}

	void Release() override
	{
		if (programId != 0) {
			glDeleteProgram(programId);
			programId = 0;
		}
		valid = false;
	}

	void Reload(bool, bool validate_) override
	{
		Link();
		if (validate_)
			Validate();
	}

	void AttachShaderObject(Shader::IShaderObject*) override
	{
	}

	const std::string& GetName() const override { return desc.name; }
	const std::string& GetLog() const override { return log; }
	bool IsValid() const override { return valid; }
	unsigned int GetObjID() const override { return programId; }

	void Draw(PrimitiveTopology topology, uint32_t firstVertex, uint32_t vertexCount) override
	{
		if (programId == 0 || vertexCount == 0)
			return;

		GLenum mode = GL_TRIANGLES;
		switch (topology) {
			case PrimitiveTopology::Triangles:     mode = GL_TRIANGLES; break;
			case PrimitiveTopology::TriangleStrip: mode = GL_TRIANGLE_STRIP; break;
			case PrimitiveTopology::Lines:         mode = GL_LINES; break;
			case PrimitiveTopology::LineStrip:     mode = GL_LINE_STRIP; break;
			case PrimitiveTopology::Points:        mode = GL_POINTS; break;
		}

		glDrawArrays(mode, static_cast<GLint>(firstVertex), static_cast<GLsizei>(vertexCount));
	}

private:
	PipelineDesc desc;
	std::string log;
	GLuint programId = 0;
	bool valid = false;
};

} // namespace

std::unique_ptr<IShaderPipeline> CreateGLStandaloneShaderPipeline(const PipelineDesc& desc)
{
	auto pipeline = std::make_unique<GLStandaloneShaderPipeline>(desc);
	pipeline->Link();
	pipeline->Validate();
	return pipeline;
}
