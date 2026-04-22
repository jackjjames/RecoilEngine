/* metal-smoke log stubs: the Metal backend sources we compile into the smoke
 * binary (MetalShaderPipeline.mm etc.) call into the engine's log frontend
 * through LOG_L(). We do not want to drag in the whole log subsystem for a
 * standalone harness, so satisfy the extern symbols by forwarding to stderr. */

#include <stdarg.h>
#include <stdio.h>

void log_frontend_record(int level, const char* section, const char* fmt, ...)
{
	(void)level;
	fprintf(stderr, "[metal-smoke:%s] ", section ? section : "");
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
}

int log_frontend_isEnabled(int level, const char* section)
{
	(void)level;
	(void)section;
	return 1;
}

void log_frontend_register_section(const char* section) { (void)section; }
void log_frontend_register_runtime_section(int level, const char* section)
{
	(void)level;
	(void)section;
}

void log_frontend_cleanup(void) {}
