#pragma once

#include <cstddef>
#include <memory>

class IBuffer;

std::unique_ptr<IBuffer> CreateGLBuffer(size_t size, const void* data = nullptr);
