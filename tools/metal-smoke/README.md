# metal-smoke

Standalone macOS smoke test for the Spring Metal rendering backend. It opens
an SDL Metal window, builds the same `TrianglePass` pipeline the engine uses
(GLSL → SPIR-V → MSL via glslang + SPIRV-Cross, `MTLRenderPipelineState`
cached), draws a single white triangle into an offscreen `MTLTexture`, reads
the center pixel back, and asserts it came out near-white.

Purpose: give us pixel-level confirmation that the Metal backend works end
to end -- shader translation, pipeline creation, command-buffer submission,
encoder record/replay of the uniform-buffer bind table, and render-target
storage -- without booting `CGlobalRendering`, `configHandler`, Lua, or
anything else that gets in the way of a fast sanity run.

The harness is only configured on `APPLE + SPRING_RENDER_BACKEND=metal` and
is `EXCLUDE_FROM_ALL`, so it never builds unless explicitly asked for.

## Build

```sh
cmake -G Ninja -B build-macos-arm64-metal -DSPRING_RENDER_BACKEND=metal
cmake --build build-macos-arm64-metal --target metal-smoke
```

## Run

```sh
./build-macos-arm64-metal/metal-smoke
```

Expected output on an Apple Silicon machine:

```
metal-smoke: device       = Apple M2
metal-smoke: drawable     = 512x512
metal-smoke: center (128,128) = R=255 G=255 B=255 A=255 -> near-white (OK)
metal-smoke: ok
```

Exit codes: `0` on success, `1` on setup failure (SDL/Metal/pipeline), `2`
on the pixel-readback gate failing.

## Scope

- Apple Silicon only. The readback path reaches straight into an
  `MTLStorageModeShared` texture after `waitUntilCompleted`; intel macs
  would need a managed-storage blit synchronize step.
- Does not link against the engine proper. It pulls in the Metal backend
  primitives it needs directly (`MetalBuffer.mm`, `MetalShaderPipeline.mm`,
  `MetalRenderGlobals.mm`, `Platform/MetalOffscreenFrame.mm`, and
  `ShaderTranslator.cpp`) plus a tiny `log_stub.c` that satisfies the
  engine's `log_frontend_*` symbols with `fprintf`.
- Not a replacement for a real integration test. The goal is a one-shot
  CLI gate that fails fast if any of the Metal-backend seams regress.
