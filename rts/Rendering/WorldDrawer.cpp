/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Stage 3 stopped short of rewriting the legacy world draw path itself. The
// backend bootstrap, presentation, render-target, and texture seams now exist;
// Stage 4 is where WorldDrawer should split pass orchestration from backend
// execution and stop expressing frame policy as raw GL calls.

#include "Rendering/GL/myGL.h"

#include "WorldDrawer.h"
#include "Rendering/IRenderTarget.h"
#include "Rendering/GL/RenderBuffers.h"
#include "Sim/Units/UnitDefHandler.h"
#include "Sim/Features/FeatureDefHandler.h"
#include "Sim/Weapons/WeaponDefHandler.h"
#include "Rendering/Env/CubeMapHandler.h"
#include "Rendering/Env/GrassDrawer.h"
#include "Rendering/Env/IGroundDecalDrawer.h"
#include "Rendering/Env/ISky.h"
#include "Rendering/Env/SunLighting.h"
#include "Rendering/Env/WaterRendering.h"
#include "Rendering/Env/MapRendering.h"
#include "Rendering/Env/IWater.h"
#include "Rendering/CommandDrawer.h"
#include "Rendering/Debug/TrianglePass.h"
#include "Rendering/DebugColVolDrawer.h"
#include "Rendering/DebugVisibilityDrawer.h"
#include "Rendering/LineDrawer.h"
#include "Rendering/LuaObjectDrawer.h"
#include "Rendering/Features/FeatureDrawer.h"
#include "Rendering/Env/Particles/ProjectileDrawer.h"
#include "Rendering/Units/UnitDrawer.h"
#include "Rendering/IPathDrawer.h"
#include "Rendering/DepthBufferCopy.h"
#include "Rendering/SmoothHeightMeshDrawer.h"
#include "Rendering/InMapDrawView.h"
#include "Rendering/ShadowHandler.h"
#include "Rendering/Map/InfoTexture/IInfoTextureHandler.h"
#include "Rendering/Models/IModelParser.h"
#include "Rendering/Models/3DModelVAO.hpp"
#include "Rendering/Models/ModelsLock.h"
#include "Rendering/Shaders/ShaderHandler.h"
#include "Rendering/Textures/ColorMap.h"
#include "Rendering/Textures/3DOTextureHandler.h"
#include "Rendering/Textures/S3OTextureHandler.h"
#include "Map/BaseGroundDrawer.h"
#include "Map/ReadMap.h"
#include "Game/Camera.h"
#include "Game/SelectedUnitsHandler.h"
#include "Game/Game.h"
#include "Game/GlobalUnsynced.h"
#include "Game/LoadScreen.h"
#include "Game/UI/CommandColors.h"
#include "Game/UI/GuiHandler.h"
#include "System/EventHandler.h"
#include "System/Exceptions.h"
#include "System/TimeProfiler.h"
#include "System/SafeUtil.h"
#include "System/Log/ILog.h"
#include "System/Config/ConfigHandler.h"
#include "System/LoadLock.h"

CONFIG(bool, PreloadModels).defaultValue(true).description("The engine will preload all models");

namespace {

IRenderTarget& GetBackbuffer()
{
	return globalRendering->renderBackend->GetRenderContext().GetDefaultRenderTarget(*globalRendering);
}

RenderTargetBlendState GetAlphaBlendState()
{
	return RenderTargetBlendState {
		.enabled = true,
		.srcColor = GL_SRC_ALPHA,
		.dstColor = GL_ONE_MINUS_SRC_ALPHA,
		.srcAlpha = GL_SRC_ALPHA,
		.dstAlpha = GL_ONE_MINUS_SRC_ALPHA,
	};
}

RenderTargetDepthState GetWorldDepthState()
{
	return RenderTargetDepthState {
		.testEnabled = true,
		.writeEnabled = true,
		.func = GL_LEQUAL,
	};
}

RenderTargetDepthState GetOverlayDepthState()
{
	return RenderTargetDepthState {
		.testEnabled = false,
		.writeEnabled = false,
		.func = GL_LEQUAL,
	};
}

void DrawColoredQuadTriangles(const VA_TYPE_C& tl, const VA_TYPE_C& tr, const VA_TYPE_C& br, const VA_TYPE_C& bl)
{
	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_C>();
	rb.AssertSubmission();

	auto& shader = rb.GetShader();
	rb.AddQuadTriangles(tl, tr, br, bl);

	shader.Enable();
	rb.DrawElements(GL_TRIANGLES);
	shader.Disable();
}

} // namespace

void CWorldDrawer::InitPre() const
{
#if defined(RENDER_BACKEND_METAL)
	// The GPU-facing pieces below (cube-map textures, fixed-function
	// GL_LIGHTs, shader programs, FBOs) are Stage 9 work. But the model
	// loader + LuaObjectDrawer are CPU-side (model geometry parsing,
	// bounding radii) and Lua scripts dereference UnitDef::radius during
	// CGame::Load, so they stay live on Metal. Texture handlers + sky +
	// feature drawer are deferred.
	LuaObjectDrawer::Init();
	CColorMap::InitStatic();
	S3DModelVAO::Init();
	modelLoader.Init();
	return;
#else
	LuaObjectDrawer::Init();

	CColorMap::InitStatic();

	// these need to be loaded before featureHandler is created
	// (maps with features have their models loaded at startup)
	S3DModelVAO::Init();
	modelLoader.Init();

	loadscreen->SetLoadMessage("Creating Unit Textures");
	textureHandler3DO.Init();
	textureHandlerS3O.Init();

	loadscreen->SetLoadMessage("Creating Sky");

	ISky::SetSky();
	sunLighting->Init();

	CFeatureDrawer::InitStatic();
#endif
}

void CWorldDrawer::InitPost() const
{
#if defined(RENDER_BACKEND_METAL)
	// See InitPre for the gating rationale. Each of these subsystems creates
	// raw GL objects. The shadow handler, info-texture handler, ground
	// decal / grass / water / sky / projectile / unit / feature drawers
	// all land incrementally as Stage 9 commits.
	return;
#endif
	char buf[512] = {0};

	CModelsLock::SetThreadSafety(true);
	const bool preloadMode = configHandler->GetBool("PreloadModels");
	{
		loadscreen->SetLoadMessage("Loading Models");

		if (preloadMode) {
			for (const auto& def : unitDefHandler->GetUnitDefsVec()) {
				def.PreloadModel();
			}

			for (const auto& def : featureDefHandler->GetFeatureDefsVec()) {
				def.PreloadModel();
			}

			for (const auto& def : weaponDefHandler->GetWeaponDefsVec()) {
				def.PreloadModel();
			}
		}
	}
	auto lock = CLoadLock::GetUniqueLock();
	{
		loadscreen->SetLoadMessage("Creating ShadowHandler");
		shadowHandler.Init();
	}
	{
		// SMFGroundDrawer accesses InfoTextureHandler, create it first
		loadscreen->SetLoadMessage("Creating InfoTextureHandler");
		IInfoTextureHandler::Create();
	}
	try {
		loadscreen->SetLoadMessage("Creating GroundDrawer");
		readMap->InitGroundDrawer();
	} catch (const content_error& e) {
		memset(buf, 0, sizeof(buf));
		snprintf(buf, sizeof(buf), "[WorldDrawer::%s] caught exception \"%s\"", __func__, e.what());
	}

	{
		loadscreen->SetLoadMessage("Creating GrassDrawer");
		grassDrawer = new CGrassDrawer();
	}
	{
		inMapDrawerView = new CInMapDrawView();
		pathDrawer = IPathDrawer::GetInstance();
	}
	{
		DepthBufferCopy::Init();
	}
	{
		IGroundDecalDrawer::Init();
	}
	{
		loadscreen->SetLoadMessage("Creating ProjectileDrawer & UnitDrawer");

		CProjectileDrawer::InitStatic();
		CUnitDrawer::InitStatic();
		// see ::InitPre
		// CFeatureDrawer::InitStatic();
	}

	// rethrow to force exit
	if (buf[0] != 0)
		throw content_error(buf);

	{
		loadscreen->SetLoadMessage("Creating Water");
		IWater::SetWater(-1);
	}
	{
		ISky::GetSky()->SetupFog();
	}
	lock = {}; //unlock
	{
		loadscreen->SetLoadMessage("Finalizing Models");
		modelLoader.DrainPreloadFutures(0);
		auto& mv = S3DModelVAO::GetInstance();
		if (preloadMode) {
			{
				auto lock = CLoadLock::GetUniqueLock();
				mv.UploadVBOs();
			}
			mv.SetSafeToDeleteVectors();
			modelLoader.LogErrors();
			CModelsLock::SetThreadSafety(false); //all models are already preloaded
		}
	}
}


void CWorldDrawer::Kill()
{
	infoTextureHandler = nullptr;

	IWater::KillWater();
	ISky::KillSky();
	spring::SafeDelete(grassDrawer);
	spring::SafeDelete(pathDrawer);
	shadowHandler.Kill();
	spring::SafeDelete(inMapDrawerView);

	CFeatureDrawer::KillStatic(gu->globalReload);
	CUnitDrawer::KillStatic(gu->globalReload); // depends on unitHandler, cubeMapHandler
	CProjectileDrawer::KillStatic(gu->globalReload);

	S3DModelVAO::Kill();
	modelLoader.Kill();

	textureHandler3DO.Kill();
	textureHandlerS3O.Kill();

	readMap->KillGroundDrawer();
	IGroundDecalDrawer::FreeInstance();
	DepthBufferCopy::Kill();
	LuaObjectDrawer::Kill();
	SmoothHeightMeshDrawer::FreeInstance();

	numUpdates = 0;
}




void CWorldDrawer::Update(bool newSimFrame)
{
	SCOPED_TIMER("Update::WorldDrawer");

	LuaObjectDrawer::Update(numUpdates == 0);
	readMap->UpdateDraw(numUpdates == 0);

	if (globalRendering->drawGround) {
		ZoneScopedN("GroundDrawer::Update");
		(readMap->GetGroundDrawer())->Update();
	}
	// XXX: done in CGame, needs to get updated even when !doDrawWorld
	// (it updates unitdrawpos which is used for maximized minimap too)
	// unitDrawer->Update();
	// lineDrawer.UpdateLineStipple();
	CUnitDrawer::UpdateStatic();
	CFeatureDrawer::UpdateStatic();
	projectileDrawer->UpdateDrawFlags();

	if (newSimFrame) {
		projectileDrawer->UpdateTextures();

		{
			SCOPED_TIMER("Update::WorldDrawer::{Sky,Water}");

			ISky::GetSky()->Update();
			IWater::GetWater()->Update();
		}

		// once every simframe is frequent enough here
		// NB: errors will not be logged until frame 0
		modelLoader.LogErrors();
	}

	numUpdates += 1;
}



void CWorldDrawer::GenerateIBLTextures() const
{

	if (shadowHandler.ShadowsLoaded()) {
		SCOPED_TIMER("Draw::World::CreateShadows");
		SCOPED_GL_DEBUGGROUP("Draw::World::CreateShadows");

		game->SetDrawMode(CGame::gameShadowDraw);
		shadowHandler.CreateShadows();
		game->SetDrawMode(CGame::gameNormalDraw);
	}

	{
		SCOPED_TIMER("Draw::World::UpdateReflTex");
		SCOPED_GL_DEBUGGROUP("Draw::World::UpdateReflTex");
		cubeMapHandler.UpdateReflectionTexture();
	}

	SCOPED_GL_DEBUGGROUP("Draw::World::UpdateMisc");
	bool sunDirUpd = ISky::GetSky()->GetLight()->Update();
	bool sunLightUpd = sunLighting->IsUpdated();
	bool skyUpd = ISky::GetSky()->IsUpdated();
	bool waterUpd = waterRendering->IsUpdated();

	if (sunDirUpd) {
		SCOPED_TIMER("Draw::World::UpdateSpecTex");
		cubeMapHandler.UpdateSpecularTexture();
	}
	if (sunDirUpd || skyUpd) {
		SCOPED_TIMER("Draw::World::UpdateSkyTex");
		ISky::GetSky()->UpdateSkyTexture();
	}
	if (sunDirUpd || sunLightUpd || waterUpd) {
		SCOPED_TIMER("Draw::World::UpdateShadingTex");
		readMap->UpdateShadingTexture();
	}
}

void CWorldDrawer::ResetMVPMatrices() const
{
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluOrtho2D(0, 1, 0, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	auto& backbuffer = GetBackbuffer();
	backbuffer.SetBlendState(GetAlphaBlendState());
	backbuffer.SetDepthState(GetOverlayDepthState());
}



void CWorldDrawer::Draw() const
{
	SCOPED_TIMER("Draw::World");
	SCOPED_GL_DEBUGGROUP("Draw::World");

	const auto& sky = ISky::GetSky();
	auto& backbuffer = GetBackbuffer();
	backbuffer.Bind();
	backbuffer.ClearColor(float4(sky->fogColor.x, sky->fogColor.y, sky->fogColor.z, 0.0f));
	backbuffer.ClearDepth(1.0f);
	glClear(GL_STENCIL_BUFFER_BIT);
	backbuffer.SetDepthState(GetWorldDepthState());
	backbuffer.SetBlendState({});

	camera->Update();
	trianglePass.Draw();

	DrawOpaqueObjects();
	DrawAlphaObjects();
	{
		SCOPED_TIMER("Draw::World::DrawWorld");
		SCOPED_GL_DEBUGGROUP("Draw::World::DrawWorld");
		eventHandler.DrawWorld();
	}


	DrawMiscObjects();
	DrawBelowWaterOverlay();

	glDisable(GL_FOG);
}


void CWorldDrawer::DrawOpaqueObjects() const
{
	CBaseGroundDrawer* gd = readMap->GetGroundDrawer();

	if (globalRendering->drawGround) {
		{
			SCOPED_TIMER("Draw::World::Terrain");
			SCOPED_GL_DEBUGGROUP("Draw::World::Terrain");
			gd->Draw(DrawPass::Normal);
			depthBufferCopy->MakeDepthBufferCopy();
		}
		{
			eventHandler.DrawPreDecals();
			SCOPED_TIMER("Draw::World::Decals");
			SCOPED_GL_DEBUGGROUP("Draw::World::Decals");
			groundDecals->Draw();
			projectileDrawer->DrawGroundFlashes();
		}
		{
			SCOPED_TIMER("Draw::World::Foliage");
			SCOPED_GL_DEBUGGROUP("Draw::World::Foliage");
			grassDrawer->Draw();
		}
		smoothHeightMeshDrawer->Draw(1.0f);
	}

	// not an opaque rendering, but makes sense to run after the terrain was rendered
	{
		const auto& sky = ISky::GetSky();
		sky->Draw();
	}

	selectedUnitsHandler.Draw();
	eventHandler.DrawWorldPreUnit();

	{
		SCOPED_TIMER("Draw::World::Models::Opaque");
		SCOPED_GL_DEBUGGROUP("Draw::World::Models::Opaque");
		unitDrawer->Draw(false);
		featureDrawer->Draw(false);
	}
	{
		SCOPED_TIMER("Draw::World::Models::Projectiles");
		SCOPED_GL_DEBUGGROUP("Draw::World::Models::Projectiles");
		projectileDrawer->DrawOpaque(false);
	}
	{
		SCOPED_TIMER("Draw::OpaqueObjects::Debug");
		SCOPED_GL_DEBUGGROUP("Draw::OpaqueObjects::Debug");
		DebugColVolDrawer::Draw();
		DebugVisibilityDrawer::DrawWorld();
		pathDrawer->DrawAll();
	}
}

void CWorldDrawer::DrawAlphaObjects() const
{
	// transparent objects
	GetBackbuffer().SetBlendState(GetAlphaBlendState());
	GetBackbuffer().SetDepthState(GetWorldDepthState());

	static const double belowPlaneEq[4] = {0.0f, -1.0f, 0.0f, 0.0f};
	static const double abovePlaneEq[4] = {0.0f,  1.0f, 0.0f, 0.0f};

	const bool hasWaterRendering = globalRendering->drawWater && readMap->HasVisibleWater();

	{
		SCOPED_TIMER("Draw::World::Models::Alpha");
		SCOPED_GL_DEBUGGROUP("Draw::World::Models::Alpha");
		// clip in model-space
		if (hasWaterRendering) {
			glPushMatrix();
			glLoadIdentity();
			glClipPlane(GL_CLIP_PLANE3, belowPlaneEq);
			glPopMatrix();
			glEnable(GL_CLIP_PLANE3);
		}

		// draw alpha-objects below water surface (farthest)
		unitDrawer->DrawAlphaPass(false);
		featureDrawer->DrawAlphaPass(false);
	}
	{
		SCOPED_TIMER("Draw::World::Particles");
		SCOPED_GL_DEBUGGROUP("Draw::World::Particles");
		projectileDrawer->DrawAlpha(!hasWaterRendering, true, false, false);

		if (hasWaterRendering)
			glDisable(GL_CLIP_PLANE3);
	}

	if (!hasWaterRendering)
		return;

	// draw water (in-between)
	{
		SCOPED_TIMER("Draw::World::Water");
		SCOPED_GL_DEBUGGROUP("Draw::World::Water");

		const auto& water = IWater::GetWater();
		{
			ZoneScopedN("Draw::World::Water::UpdateWater");
			water->UpdateWater(game);
		}
		water->Draw();
		eventHandler.DrawWaterPost();
	}

	{
		SCOPED_TIMER("Draw::World::Models::Alpha");
		SCOPED_GL_DEBUGGROUP("Draw::World::Alpha");
		glPushMatrix();
		glLoadIdentity();
		glClipPlane(GL_CLIP_PLANE3, abovePlaneEq);
		glPopMatrix();
		glEnable(GL_CLIP_PLANE3);

		// draw alpha-objects above water surface (closest)
		unitDrawer->DrawAlphaPass(false);
		featureDrawer->DrawAlphaPass(false);
	}
	{
		SCOPED_TIMER("Draw::World::Particles");
		SCOPED_GL_DEBUGGROUP("Draw::World::Particles");
		projectileDrawer->DrawAlpha(true, false, false, false);

		glDisable(GL_CLIP_PLANE3);
	}
}

void CWorldDrawer::DrawMiscObjects() const
{

	{
		// note: duplicated in CMiniMap::DrawWorldStuff()
		commandDrawer->DrawLuaQueuedUnitSetCommands();

		if (cmdColors.AlwaysDrawQueue() || guihandler->GetQueueKeystate()) {
			selectedUnitsHandler.DrawCommands();
		}
	}

	// either draw from here, or make {Dyn,Bump}Water use blending
	// pro: icons are drawn only once per frame, not every pass
	// con: looks somewhat worse for underwater / obscured icons
	if (!CUnitDrawer::UseScreenIcons())
		unitDrawer->DrawUnitIcons();

	lineDrawer.DrawAll();
	cursorIcons.Draw();

	mouse->DrawSelectionBox();
	guihandler->DrawMapStuff(false);

	if (globalRendering->drawMapMarks && !game->hideInterface) {
		inMapDrawerView->Draw();
	}
}



void CWorldDrawer::DrawBelowWaterOverlay() const
{

	if (!globalRendering->drawWater)
		return;
	if (mapRendering->voidWater)
		return;
	if (camera->GetPos().y >= 0.0f)
		return;

	{
		const float3& cpos = camera->GetPos();
		const float vr = camera->GetFarPlaneDist() * 0.5f;
		const SColor surfaceColor(0.0f, 0.5f, 0.3f, 0.50f);
		const SColor wallColor(0.0f, 0.5f, 0.3f, 0.50f);

		GetBackbuffer().SetBlendState(GetAlphaBlendState());
		GetBackbuffer().SetDepthState(GetOverlayDepthState());

		{
			DrawColoredQuadTriangles(
				{{cpos.x - vr, 0.0f, cpos.z - vr}, surfaceColor},
				{{cpos.x + vr, 0.0f, cpos.z - vr}, surfaceColor},
				{{cpos.x + vr, 0.0f, cpos.z + vr}, surfaceColor},
				{{cpos.x - vr, 0.0f, cpos.z + vr}, surfaceColor}
			);
		}

		{
			DrawColoredQuadTriangles(
				{{cpos.x - vr, 0.0f, cpos.z - vr}, wallColor},
				{{cpos.x + vr, 0.0f, cpos.z - vr}, wallColor},
				{{cpos.x + vr, -vr, cpos.z - vr}, wallColor},
				{{cpos.x - vr, -vr, cpos.z - vr}, wallColor}
			);
			DrawColoredQuadTriangles(
				{{cpos.x + vr, 0.0f, cpos.z - vr}, wallColor},
				{{cpos.x + vr, 0.0f, cpos.z + vr}, wallColor},
				{{cpos.x + vr, -vr, cpos.z + vr}, wallColor},
				{{cpos.x + vr, -vr, cpos.z - vr}, wallColor}
			);
			DrawColoredQuadTriangles(
				{{cpos.x + vr, 0.0f, cpos.z + vr}, wallColor},
				{{cpos.x - vr, 0.0f, cpos.z + vr}, wallColor},
				{{cpos.x - vr, -vr, cpos.z + vr}, wallColor},
				{{cpos.x + vr, -vr, cpos.z + vr}, wallColor}
			);
			DrawColoredQuadTriangles(
				{{cpos.x - vr, 0.0f, cpos.z + vr}, wallColor},
				{{cpos.x - vr, 0.0f, cpos.z - vr}, wallColor},
				{{cpos.x - vr, -vr, cpos.z - vr}, wallColor},
				{{cpos.x - vr, -vr, cpos.z + vr}, wallColor}
			);
		}
	}

	{
		// draw water-coloration quad in raw screenspace
		ResetMVPMatrices();

		DrawColoredQuadTriangles(
			{{0.0f, 0.0f, -1.0f}, SColor(0.0f, 0.2f, 0.8f, 0.333f)},
			{{1.0f, 0.0f, -1.0f}, SColor(0.0f, 0.2f, 0.8f, 0.333f)},
			{{1.0f, 1.0f, -1.0f}, SColor(0.0f, 0.2f, 0.8f, 0.333f)},
			{{0.0f, 1.0f, -1.0f}, SColor(0.0f, 0.2f, 0.8f, 0.333f)}
		);
	}
}
