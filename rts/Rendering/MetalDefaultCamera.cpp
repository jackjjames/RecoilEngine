/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalDefaultCamera.h"

#include "Game/CameraHandler.h"
#include "Game/Camera/CameraController.h"
#include "Game/GlobalUnsynced.h"
#include "Sim/Misc/TeamHandler.h"
#include "System/float3.h"

namespace MetalDefaultCamera {

void MaybeSnapToStartPos()
{
	static bool didSnap = false;
	if (didSnap)
		return;

	if (camHandler == nullptr || gu == nullptr)
		return;
	if (!teamHandler.IsValidTeam(gu->myTeam))
		return;

	const float3& sp = teamHandler.Team(gu->myTeam)->GetStartPos();
	if (sp.x <= 0.0f && sp.z <= 0.0f)
		return; // Start position not assigned yet; try again next frame.

	// SpringController treats SetPos as the *look target* (focus =
	// pos, eye = pos - dir*curDist), so the snap actually centres the
	// start on screen. The default 'rot' controller treats SetPos as
	// the eye, which leaves the unit behind the frustum.
	camHandler->SetCameraMode("spring");
	CCameraController& cc = camHandler->GetCurrentController();
	cc.SetPos(sp);
	// SpringController seeds curDist from map size (~Length2D(mapx,
	// mapy) * 12), which on a 600x600 map shrinks a commander to a
	// sub-10-pixel speck. Pull in to a fixed zoom so units are
	// recognisable at game start; this matches what BAR's Lua UI
	// does on the GL side. Goes away with S9-C6.
	CCameraController::StateMap sm;
	cc.GetState(sm);
	// Pull in to ~700 units so a commander reads at a recognisable
	// silhouette (tens of px tall) instead of a sub-10-px speck.
	// Anything closer starts clipping pieces against the near plane
	// on tall units like factories.
	sm["dist"] = 700.0f;
	// rot.x = pitch from world up; SpringController clamps to
	// (PI*0.51, PI*0.99). PI*0.62 ~= 1.95 rad gives a slightly
	// flatter 3/4 isometric than 2.20 - reads more like the GL
	// build's default skirmish camera.
	sm["rx"] = 1.95f;
	cc.SetState(sm);
	camHandler->CameraTransition(0.0f);
	didSnap = true;
}

} // namespace MetalDefaultCamera

#endif // RENDER_BACKEND_METAL
