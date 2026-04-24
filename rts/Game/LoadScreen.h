/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef _LOAD_SCREEN_H
#define _LOAD_SCREEN_H

#include <string>

#include "GameController.h"
#include "System/LoadSave/LoadSaveHandler.h"
#include "System/GameLoadThread.h"
#include "System/Misc/SpringTime.h"
#include "System/Threading/SpringThreading.h"

class CglFont;

class CLoadScreen : public CGameController
{
public:
	void SetLoadMessage(const std::string& text, bool replaceLast = false);

	CLoadScreen(std::string&& mapFileName, std::string&& modFileName, ILoadSaveHandler* saveFile);
	~CLoadScreen();

	bool Init(); /// split from ctor; uses GetInstance()
	void Kill(); /// split from dtor

public:
	// singleton can potentially be null, but only when
	// accessed from Game::KillLua where we do not care
	static CLoadScreen* GetInstance() { return singleton; }

	static void CreateDeleteInstance(std::string&& mapFileName, std::string&& modFileName, ILoadSaveHandler* saveFile);
	static bool CreateInstance(std::string&& mapFileName, std::string&& modFileName, ILoadSaveHandler* saveFile);
	static void DeleteInstance();

	// Opportunistic pump for long synchronous load steps that don't hit SetLoadMessage().
	// Throttled to ~30Hz internally; safe to call hot, no-op off the main thread.
	// Ticks WDT_MAIN, pumps SDL, redraws the load screen.
	static void TickMain();

	bool Draw() override;
	bool Update() override;

	void ResizeEvent() override;

	int KeyReleased(int keyCode, int scanCode) override;
	int KeyPressed(int keyCode, int scanCode, bool isRepeat) override;


private:
	static CLoadScreen* singleton;

	ILoadSaveHandler* saveFile;

	std::vector< std::pair<std::string, bool> > loadMessages;

	// Most recent SetLoadMessage text, cached for the Metal text overlay
	// which paints it every frame while CFontTexture still targets GL.
	// Updated under `mutex`.
	std::string lastLoadMessage;

	std::string mapFileName;
	std::string modFileName;

	spring::recursive_mutex mutex;
	spring::thread netHeartbeatThread;
	CGameLoadThread gameLoadThread;

	bool mtLoading;

	spring_time lastDrawTime;
};


#define loadscreen CLoadScreen::GetInstance()


#endif // _LOAD_SCREEN_H
