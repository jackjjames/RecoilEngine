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
	camHandler->GetCurrentController().SetPos(sp);
	camHandler->CameraTransition(0.0f);
	didSnap = true;
}

} // namespace MetalDefaultCamera

#endif // RENDER_BACKEND_METAL
