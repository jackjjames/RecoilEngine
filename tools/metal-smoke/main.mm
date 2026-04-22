// metal-smoke: standalone harness that will exercise the engine's Metal
// backend bits in isolation, without booting CGlobalRendering / configHandler
// / the full Spring app. Stage 6 grows it slice-by-slice until a single
// triangle draws into an offscreen texture and a center-pixel readback
// confirms pixels landed. This first slice is just an empty CLI target so
// later commits can stack cleanly on top.

#include <cstdio>

int main()
{
	std::puts("metal-smoke: hello");
	return 0;
}
