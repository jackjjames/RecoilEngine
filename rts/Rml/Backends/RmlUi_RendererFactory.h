/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <memory>

namespace RmlGui
{
	class IRmlRendererBackend
	{
	public:
		virtual ~IRmlRendererBackend() = default;

		virtual bool IsValid() const = 0;
		virtual Rml::RenderInterface* GetRenderInterface() = 0;
		virtual void SetViewport(int width, int height) = 0;
		virtual void BeginFrame() = 0;
		virtual void EndFrame() = 0;
	};

	class RenderBackend
	{
	public:
		RenderBackend();
		~RenderBackend();

		explicit operator bool() const;

		Rml::RenderInterface* GetRenderInterface();
		void SetViewport(int width, int height);
		void BeginFrame();
		void EndFrame();

	private:
		std::unique_ptr<IRmlRendererBackend> impl;
	};
} // namespace RmlGui
