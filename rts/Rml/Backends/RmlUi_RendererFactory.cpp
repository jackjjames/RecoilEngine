/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "RmlUi_RendererFactory.h"

#ifndef HEADLESS
#include "RmlUi_Renderer_GL3_Recoil.h"
#else
#include "RmlUi_Renderer_Headless.h"
#endif

namespace RmlGui
{
#ifndef HEADLESS
	struct GL3RmlRendererBackend final : IRmlRendererBackend
	{
		bool IsValid() const override { return static_cast<bool>(renderInterface); }
		Rml::RenderInterface* GetRenderInterface() override { return &renderInterface; }
		void SetViewport(int width, int height) override { renderInterface.SetViewport(width, height); }
		void BeginFrame() override { renderInterface.BeginFrame(); }
		void EndFrame() override { renderInterface.EndFrame(); }

		RenderInterface_GL3_Recoil renderInterface;
	};
#else
	struct HeadlessRmlRendererBackend final : IRmlRendererBackend
	{
		bool IsValid() const override { return static_cast<bool>(renderInterface); }
		Rml::RenderInterface* GetRenderInterface() override { return &renderInterface; }
		void SetViewport(int width, int height) override { renderInterface.SetViewport(width, height); }
		void BeginFrame() override { renderInterface.BeginFrame(); }
		void EndFrame() override { renderInterface.EndFrame(); }

		RenderInterface_Headless renderInterface;
	};
#endif

	RenderBackend::RenderBackend()
#ifndef HEADLESS
		: impl(std::make_unique<GL3RmlRendererBackend>())
#else
		: impl(std::make_unique<HeadlessRmlRendererBackend>())
#endif
	{
	}

	RenderBackend::~RenderBackend() = default;

	RenderBackend::operator bool() const
	{
		return (impl != nullptr && impl->IsValid());
	}

	Rml::RenderInterface* RenderBackend::GetRenderInterface()
	{
		return impl->GetRenderInterface();
	}

	void RenderBackend::SetViewport(int width, int height)
	{
		impl->SetViewport(width, height);
	}

	void RenderBackend::BeginFrame()
	{
		impl->BeginFrame();
	}

	void RenderBackend::EndFrame()
	{
		impl->EndFrame();
	}
} // namespace RmlGui
