#pragma once

#include <memory>
#include <array>
#include <cstdint>

#include "Rendering/Textures/ITexture.h"
#include "Rendering/Textures/NullTexture.h"
#include "System/UnorderedSet.hpp"
#include "System/EventClient.h"
#include "Rendering/GL/FBO.h"

struct ScopedDepthBufferCopy;

class DepthBufferCopy : public CEventClient {
public:
	friend struct ScopedDepthBufferCopy;
	DepthBufferCopy();
	~DepthBufferCopy() override;

	static void Init();
	static void Kill();

	bool WantsEvent(const std::string& eventName) override {
		return
			(eventName == "ViewResize");
	}

	void ViewResize() override;

	bool IsValid(bool ms) const;

	void MakeDepthBufferCopy() const;
	uint32_t GetDepthBufferTexture(bool ms) const { return depthTextures[ms]; }
	ITexture& GetDepthBufferTextureHandle(bool ms) const { return (depthTextureHandles[ms] != nullptr) ? *depthTextureHandles[ms] : GetNullTexture(); }
private:
	// to be accessed with ScopedDepthBufferCopy
	void AddConsumer(bool ms);
	void DelConsumer(bool ms);
private:
	void DestroyTextureAndFBO(bool ms);
	void CreateTextureAndFBO(bool ms);
	void RecreateTextureAndFBO(bool ms) {
		DestroyTextureAndFBO(ms);
		CreateTextureAndFBO(ms);
	}

	std::array<uint32_t, 2> consumersCount = {};
	std::array<uint32_t, 2> depthTextures = {};
	mutable std::array<std::unique_ptr<ITexture>, 2> depthTextureHandles = {};
	mutable std::array<uint32_t, 2> depthTextureHandleIds = {};
	std::array<std::unique_ptr<FBO>, 2> depthFBOs = {};
};

extern std::unique_ptr<DepthBufferCopy> depthBufferCopy;

struct ScopedDepthBufferCopy {
	ScopedDepthBufferCopy(bool ms)
		: multisampled(ms)
	{
		depthBufferCopy->AddConsumer(multisampled);
	}
	ScopedDepthBufferCopy(const ScopedDepthBufferCopy&) = delete;
	ScopedDepthBufferCopy& operator = (const ScopedDepthBufferCopy&) = delete;

	ScopedDepthBufferCopy(ScopedDepthBufferCopy&& other) noexcept { *this = std::move(other); }
	ScopedDepthBufferCopy& operator = (ScopedDepthBufferCopy&& other) noexcept {
		std::swap(multisampled, other.multisampled);
		return *this;
	}

	~ScopedDepthBufferCopy()
	{
		depthBufferCopy->DelConsumer(multisampled);
	}
	bool multisampled;
};