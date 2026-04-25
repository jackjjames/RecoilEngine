/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

namespace MetalDefaultCamera {

// One-shot bring-up snap. The default camera controller (typically
// 'rot' from CamModeName) seeds its eye at map centre, which leaves
// the local team's commander far outside the frustum on most BAR
// maps. In the GL build BAR's Lua UI repositions the camera once
// the game starts; that UI isn't ported on Metal yet (S9-C6), so we
// stand in with a single C++ snap that puts a 'spring' (look-at)
// camera over the local start position. Goes away when RmlUI lands
// and the Lua side takes over again.
//
// Cheap: idempotent, returns immediately once snapped, otherwise
// just queries CGameSetup / teamHandler. Safe to call every frame
// from the Metal Draw path.
void MaybeSnapToStartPos();

} // namespace MetalDefaultCamera

#endif // RENDER_BACKEND_METAL
