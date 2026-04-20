/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <memory>

namespace RmlGui
{
	class RenderBackend
	{
	public:
		struct Impl;

		RenderBackend();
		~RenderBackend();

		explicit operator bool() const;

		Rml::RenderInterface* GetRenderInterface();
		void SetViewport(int width, int height);
		void BeginFrame();
		void EndFrame();

	private:
		std::unique_ptr<Impl> impl;
	};
} // namespace RmlGui
