/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <SDL.h>
#include <cstdlib>
#include <functional>

#include "Rendering/GL/myGL.h"
#include "LoadScreen.h"
#include "Game.h"
#include "GlobalUnsynced.h"
#include "Game/Players/Player.h"
#include "Game/Players/PlayerHandler.h"
#include "Game/UI/MouseHandler.h"
#include "ExternalAI/SkirmishAIHandler.h"
#include "Lua/LuaIntro.h"
#include "Lua/LuaMenu.h"
#include "Map/MapInfo.h"
#include "Rendering/Fonts/glFont.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/Textures/NamedTextures.h"
#if defined(RENDER_BACKEND_METAL)
#include "Rendering/MetalSplashRenderer.h"
#include "Rendering/MetalTextOverlay.h"
#endif
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Path/IPathManager.h"
#include "System/Config/ConfigHandler.h"
#include "System/Exceptions.h"
#include "System/Sync/FPUCheck.h"
#include "System/Log/ILog.h"
#include "Net/Protocol/NetProtocol.h"
#include "System/Misc/UnfreezeSpring.h"
#include "System/Matrix44f.h"
#include "System/SafeUtil.h"
#include "System/FileSystem/FileHandler.h"
#include "System/Platform/Watchdog.h"
#include "System/Platform/Threading.h"
#include "System/Sound/ISound.h"
#include "System/Sound/ISoundChannels.h"
#include "System/LoadLock.h"

#if !defined(HEADLESS) && !defined(NO_SOUND)
#include "System/Sound/OpenAL/EFX.h"
#include "System/Sound/OpenAL/EFXPresets.h"
#endif

#include <vector>

#include "System/Misc/TracyDefs.h"

CONFIG(int, LoadingMT)
	.description("Experimental option to load the game in separate thread. Expect visual glitches, crashes and deadlocks")
	.defaultValue(0)
	.safemodeValue(0);


CLoadScreen* CLoadScreen::singleton = nullptr;

CLoadScreen::CLoadScreen(std::string&& _mapFileName, std::string&& _modFileName, ILoadSaveHandler* _saveFile)
	: saveFile(_saveFile)

	, mapFileName(std::move(_mapFileName))
	, modFileName(std::move(_modFileName))

	, mtLoading(true)
	, lastDrawTime(0)
{
}

CLoadScreen::~CLoadScreen()
{
	RECOIL_DETAILED_TRACY_ZONE;
	// Kill() must have been called first, such that the loading
	// thread can not access singleton while its dtor is running
	assert(!gameLoadThread.joinable());

	if (clientNet != nullptr)
		clientNet->KeepUpdating(false);
	if (netHeartbeatThread.joinable())
		netHeartbeatThread.join();

	if (!gu->globalQuit) {
		activeController = game;

		if (luaMenu != nullptr)
			luaMenu->ActivateGame();
	}

	if (activeController == this)
		activeController = nullptr;
}


bool CLoadScreen::Init()
{
	RECOIL_DETAILED_TRACY_ZONE;
	activeController = this;

	// When calling this function, mod archives have to be loaded
	// and gu->myPlayerNum has to be set.
	skirmishAIHandler.LoadPreGame();


#ifdef HEADLESS
	mtLoading = false;
#else
	const int mtCfg = configHandler->GetInt("LoadingMT");
	// user override
	mtLoading = (mtCfg > 0);
#endif


	// Create a thread during the loading that pings the host/server, so it knows that this client is still alive/loading
	clientNet->KeepUpdating(true);

	netHeartbeatThread = spring::thread(Threading::CreateNewThread(std::bind(&CNetProtocol::UpdateLoop, clientNet)));
	game = new CGame(mapFileName, modFileName, saveFile);

	CglFont::sync.SetThreadSafety(mtLoading);
	CLoadLock::SetThreadSafety(mtLoading);
	if (mtLoading) {
		try {
			// create the game-loading thread; rebinds primary context to hidden window
			gameLoadThread = CGameLoadThread(std::bind(&CGame::Load, game, mapFileName));

			while (!Watchdog::HasThread(WDT_LOAD));
		} catch (const opengl_error& gle) {
			LOG_L(L_WARNING, "[LoadScreen::%s] offscreen GL context creation failed (error: \"%s\")", __func__, gle.what());

			mtLoading = false;
			CglFont::sync.SetThreadSafety(false);
			CLoadLock::SetThreadSafety(false);
		}
	}

	// LuaIntro must be loaded and killed in the same thread (main)
	// and bound context (secondary) that CLoadScreen::Draw runs in
	//
	// note that it has access to gl.LoadFont (which creates a user
	// data wrapping a local font) but also to gl.*Text (which uses
	// the global font), the latter will cause problems in GL4
	{
		auto lock = CLoadLock::GetUniqueLock();
		CLuaIntro::LoadFreeHandler();
	}

	if (mtLoading)
		return true;

	LOG("[LoadScreen::%s] single-threaded", __func__);
	// Single-threaded load blocks the main thread for minutes (Lua defs,
	// model preload). Update()/SetLoadMessage() ticks WDT_MAIN via
	// UnfreezeSpring() and pumps SDL, so no Deregister/Register dance is
	// needed here as long as load phases call SetLoadMessage periodically.
	game->Load(mapFileName);
	return false;
}

void CLoadScreen::Kill()
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (mtLoading && !gameLoadThread.joinable())
		return;

	if (luaIntro != nullptr)
		luaIntro->Shutdown();

	CLuaIntro::FreeHandler();

	// at this point, gameLoadThread running CGame::Load
	// has finished and deregistered itself from WatchDog
	gameLoadThread.join();

	CFontTexture::sync.SetThreadSafety(false);
	CLoadLock::SetThreadSafety(false);
#if defined(RENDER_BACKEND_METAL)
	// GL context / GL_MULTISAMPLE hand-off doesn't apply: the Metal
	// context stays current for the lifetime of the process and glad
	// entry points are null (gladLoadGL is gated on the SDL GL render
	// context, which Metal bypasses). Skip to avoid a null-deref on
	// glad_glEnable during the load -> game transition.
#else
	// set last time and forever
	globalRendering->MakeCurrentContext(false);
	globalRendering->ToggleMultisampling();
#endif
}


/******************************************************************************/

static void FinishedLoading()
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (gu->globalQuit)
		return;

	// send our playername to the server to indicate we finished loading
	const CPlayer* p = playerHandler.Player(gu->myPlayerNum);

	clientNet->Send(CBaseNetProtocol::Get().SendPlayerName(gu->myPlayerNum, p->name));
	#ifdef SYNCCHECK
	clientNet->Send(CBaseNetProtocol::Get().SendPathCheckSum(gu->myPlayerNum, pathManager->GetPathCheckSum()));
	#endif
	mouse->ShowMouse();

	#if !defined(HEADLESS) && !defined(NO_SOUND)
	// NB: sound is initialized at this point, but EFX support is *not* guaranteed
	efx.CommitEffects(mapInfo->efxprops);
	#endif
}


void CLoadScreen::CreateDeleteInstance(std::string&& mapFileName, std::string&& modFileName, ILoadSaveHandler* saveFile)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (CreateInstance(std::move(mapFileName), std::move(modFileName), saveFile))
		return;

	// not mtLoading, Game::Load has completed and we can go
	DeleteInstance();
	FinishedLoading();
}

bool CLoadScreen::CreateInstance(std::string&& mapFileName, std::string&& modFileName, ILoadSaveHandler* saveFile)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(singleton == nullptr);
	singleton = new CLoadScreen(std::move(mapFileName), std::move(modFileName), saveFile);

	// returns true when mtLoading, false otherwise
	return (singleton->Init());
}

void CLoadScreen::DeleteInstance()
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (singleton == nullptr)
		return;

	singleton->Kill();
	spring::SafeDelete(singleton);
}


void CLoadScreen::TickMain()
{
	CLoadScreen* ls = singleton;
	if (ls == nullptr || ls->mtLoading)
		return;
	// Only meaningful from the main thread; the watchdog/SDL APIs we use are main-thread-only.
	if (!Threading::IsMainThread())
		return;

	static spring_time lastTick = spring_gettime();
	const spring_time now = spring_gettime();
	if (spring_tomsecs(now - lastTick) < 33)
		return;
	lastTick = now;

	// Guard against re-entry: Draw() can call luaMenu->Update() which runs
	// Lua; that would fire our count-hook again and recurse into TickMain.
	static thread_local bool inTick = false;
	if (inTick)
		return;
	inTick = true;

	spring::UnfreezeSpring(WDT_MAIN);
	ls->Draw();

	inTick = false;
}


/******************************************************************************/

void CLoadScreen::ResizeEvent()
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (luaIntro != nullptr)
		luaIntro->ViewResize();
}


int CLoadScreen::KeyPressed(int keyCode, int scanCode, bool isRepeat)
{
	RECOIL_DETAILED_TRACY_ZONE;
	//FIXME add mouse events
	if (luaIntro != nullptr)
		luaIntro->KeyPress(keyCode, scanCode, isRepeat);

	return 0;
}

int CLoadScreen::KeyReleased(int keyCode, int scanCode)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (luaIntro != nullptr)
		luaIntro->KeyRelease(keyCode, scanCode);

	return 0;
}


bool CLoadScreen::Update()
{
	ZoneScoped;

	if (luaIntro != nullptr) {
		// keep checking this while we are the active controller
		std::lock_guard<spring::recursive_mutex> lck(mutex);

		for (const auto& pair: loadMessages) {
			good_fpu_control_registers(pair.first.c_str());
			luaIntro->LoadProgress(pair.first, pair.second);
		}

		loadMessages.clear();
	}

	if (game->IsDoneLoading()) {
		CLoadScreen::DeleteInstance();
		FinishedLoading();
		return true;
	}

	// without this call the window manager would think the window is unresponsive and thus ask for hard kill.
	// tick WDT_MAIN (not WDT_LOAD) on the single-threaded path: this thread *is* the main thread, and WDT_LOAD
	// is only registered for the offscreen load worker in mtLoading mode.
	if (!mtLoading)
		spring::UnfreezeSpring(WDT_MAIN);

	return true;
}


bool CLoadScreen::Draw()
{
	RECOIL_DETAILED_TRACY_ZONE;
	// limit FPS via sleep to not lock a singlethreaded CPU from loading the game
	if (mtLoading) {
		const spring_time now = spring_gettime();
		const unsigned diffTime = spring_tomsecs(now - lastDrawTime);

		constexpr unsigned wantedFPS = 50;
		constexpr unsigned minFrameTime = 1000 / wantedFPS;

		if (diffTime < minFrameTime)
			spring_sleep(spring_msecs(minFrameTime - diffTime));

		lastDrawTime = now;
	}

	globalRendering->drawFrame = std::max(1U, globalRendering->drawFrame + 1);
	// let LuaMenu keep the lobby connection alive
	if (luaMenu != nullptr)
		luaMenu->Update();

	if (luaIntro != nullptr) {
		luaIntro->Update();
#if !defined(RENDER_BACKEND_METAL)
		// Metal has no GL clear + gl.* widget pipeline yet; let the presenter
		// own the frame. LoadScreen widgets still get updated so their state
		// machines stay alive. Real Metal loading visuals land with S8-C5 +
		// S10-C* (gl.* shim).
		luaIntro->DrawGenesis();
		ClearScreen();
		luaIntro->DrawLoadScreen();
#endif
	}

	if (!mtLoading) {
#if defined(RENDER_BACKEND_METAL)
		// On GL the inline ClearScreen() above acts as the frame body and
		// SwapBuffers presents it. On Metal we have no active drawable until
		// BeginFrame is called, so SwapBuffers alone would commit nothing.
		// Lazy-init a MetalSplashRenderer on the first draw and reuse it
		// for every load-screen frame so the window shows a tile/image
		// instead of a black surface during the multi-minute load. The
		// text overlay paints the most recent SetLoadMessage() text via a
		// tiny 5x7 bitmap font; full CglFont support lands later.
		static MetalSplashRenderer loadTile;
		static MetalTextOverlay    textOverlay;

		std::string msgCopy;
		{
			std::lock_guard<spring::recursive_mutex> lck(mutex);
			msgCopy = lastLoadMessage;
		}

		globalRendering->BeginFrame();
		loadTile.Draw();
		if (!msgCopy.empty()) {
			// Centered-ish near the bottom of the viewport, above the
			// splash tile so the text does not get visually clipped.
			constexpr float glyphH = 0.035f;
			textOverlay.DrawLine(-0.95f, -0.70f, glyphH, msgCopy);
		}
		globalRendering->PresentFrame(true, true);

		// The main thread is otherwise blocked inside CGame::Load between
		// SetLoadMessage() calls. Pump SDL here so the window acknowledges
		// traffic-light clicks. SDL_QUIT / close -> hard exit; we have no
		// clean teardown path mid-load yet.
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT ||
			    (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
				std::exit(0);
			}
		}
#else
		globalRendering->SwapBuffers(true, false);
#endif
	}

	return true;
}


/******************************************************************************/
/******************************************************************************/

void CLoadScreen::SetLoadMessage(const std::string& text, bool replaceLast)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// tick the correct watchdog: WDT_LOAD when on the loader thread, WDT_MAIN on single-threaded path.
	spring::UnfreezeSpring(mtLoading ? WDT_LOAD : WDT_MAIN);

	std::lock_guard<spring::recursive_mutex> lck(mutex);

	loadMessages.emplace_back(text, replaceLast);
	lastLoadMessage = text;

	LOG("[LoadScreen::%s] text=\"%s\"", __func__, text.c_str());
	LOG_CLEANUP();

	// be paranoid about FPU state for the loading thread since some
	// external library might reset it (main thread state is checked
	// in ::Update)
	good_fpu_control_registers(text.c_str());

	if (mtLoading)
		return;

	Update();
	Draw();
}

