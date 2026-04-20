/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "RmlUi_RendererFactory.h"

#ifndef HEADLESS
#include "RmlUi_Renderer_GL3_Recoil.h"
#else
#include "RmlUi_Renderer_Headless.h"
#endif

namespace RmlGui
{
	struct RenderBackend::Impl
	{
		virtual ~Impl() = default;

		virtual bool IsValid() const = 0;
		virtual Rml::RenderInterface* GetRenderInterface() = 0;
		virtual void SetViewport(int width, int height) = 0;
		virtual void BeginFrame() = 0;
		virtual void EndFrame() = 0;
	};

#ifndef HEADLESS
	struct GL3RenderBackend final : RenderBackend::Impl
	{
		bool IsValid() const override { return static_cast<bool>(renderInterface); }
		Rml::RenderInterface* GetRenderInterface() override { return &renderInterface; }
		void SetViewport(int width, int height) override { renderInterface.SetViewport(width, height); }
		void BeginFrame() override { renderInterface.BeginFrame(); }
		void EndFrame() override { renderInterface.EndFrame(); }

		RenderInterface_GL3_Recoil renderInterface;
	};
#else
	struct HeadlessRenderBackend final : RenderBackend::Impl
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
		: impl(std::make_unique<GL3RenderBackend>())
#else
		: impl(std::make_unique<HeadlessRenderBackend>())
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
