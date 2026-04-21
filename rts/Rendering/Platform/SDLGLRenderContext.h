/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <memory>

class IRenderContext;

std::unique_ptr<IRenderContext> CreateSDLGLRenderContext();
