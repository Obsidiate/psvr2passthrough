# Design notes

## Two front-ends, one core

The runtime-agnostic engine lives in static libs with **no OpenXR or OpenVR
includes**: `psvr2pt_core` (camera ingest, calibration, FOV/IPD math,
undistortion mesh, input), `psvr2pt_render` (the D3D11 `Compositor`), and
`psvr2pt_config`. Two thin front-ends consume them:

- **OpenXR API layer** (`src/layer/`, `PSVR2PassthroughLayer.dll`) — hooks
  `xrEndFrame`, submits an `XrCompositionLayerProjection`. Used by OpenXR games.
- **OpenVR overlay app** (`src/overlay/`, `PSVR2PassthroughOverlay.exe`) — a
  standalone `VRApplication_Overlay` process that submits a head-locked,
  side-by-side overlay texture via `IVROverlay`. Composites over *any* SteamVR
  game, so it reaches native-OpenVR titles the layer can't attach to.

The `Compositor` renders each eye into a standalone RGBA texture, so neither
front-end is coupled to it: the layer `CopyResource`s into OpenXR swapchains; the
overlay copies both eyes side-by-side and calls `SetOverlayTexture`.

## Why keep both front-ends (the overlay does not subsume the layer)

SteamVR is always running while the PSVR2 driver is active, so the OpenVR overlay
*can* technically draw over OpenXR titles too. It is tempting to conclude the
layer is redundant. It is not — the two submit fundamentally different primitives,
and for OpenXR titles the layer is strictly better:

- **Layer → `XrCompositionLayerProjection`.** Passthrough is composited in the
  *same projected space* as the game, using the game's own per-eye pose and a
  camera-derived FOV, and rides the runtime's asynchronous reprojection (ATW)
  frame-for-frame with the game. See `compose_layer()` in
  `src/layer/layer_session.cpp`.
- **Overlay → `IVROverlay` quad.** A head-locked billboard at a fixed virtual
  distance, sized to fill the camera frustum. SteamVR reprojects it, but as a flat
  quad pinned to the headset — not a true per-eye projection sharing the game's
  frustum. (This is exactly the quad-vs-projection trade-off described below.)

Concretely, the layer gives OpenXR titles three things the overlay cannot:

1. **Per-eye projection fidelity** — composited in the game's projected space, not
   as a billboard; better depth agreement, less "floating screen" feel.
2. **Quad-views (foveated) support** — the layer handles `viewCount == 4`
   (DCS with Quad-Views-Foveated); an overlay billboard has no notion of the
   game's quad-views render.
3. **Tighter reprojection coupling** — it rides the game's ATW using the game's
   pose, rather than being reprojected as an independent billboard.

The overlay's advantage is the mirror image: because it never touches the game's
frame (it owns its own D3D11 device and submits a finished texture), it is
**API-agnostic** and reaches native-OpenVR games the layer can never attach to.
That independence is *why* it can't match projection fidelity.

So the split is deliberate: each title uses its best path — OpenXR titles
(DCS, MSFS) get the layer; native-OpenVR titles (Alyx, No Man's Sky) get the
overlay. Collapsing to overlay-only is viable and would cut maintenance (no
`xrEndFrame` hook, no D3D11On12 interop, one binary), but it would downgrade the
primary, most-iterated target (DCS) to billboard-quality passthrough and drop
quad-views support — so both are kept.

## Pipeline

```
PSVR2 driver (SteamVR)
    │  named shared memory + event + mutex
    ▼
psvr2pt_core::CameraSource   ── producer thread, 60 Hz
    │ StereoFrame (1024×1024 BC4 → R8, per eye)
    ▼
psvr2pt_render::Compositor
    • upload grayscale → R8 textures
    • undistort + mounting-angle rectify via per-eye mesh
    • premultiplied-alpha output (R8G8B8A8)
    ▼
PSVR2PassthroughLayer.dll (xrEndFrame hook)
    • BindingPoller gates visibility (keyboard / XInput / DInput)
    • Acquires per-eye swapchain matched to game's projection-view extent
    • CopyResource composited eye texture → swapchain image
    • Prepends XrCompositionLayerProjection using game's eye pose + FOV
    ▼
OpenXR runtime (SteamVR)
    ▼
PSVR2 headset
```

## Why a projection layer, not a quad layer

Quad layers (`XR_TYPE_COMPOSITION_LAYER_QUAD`) are simpler but they live at a
fixed pose in world space and don't get per-eye reprojection from the runtime.
That makes them lag visibly when you move your head, and they can't track the
runtime's view transform between `xrLocateViews` and the actual display refresh.

A projection layer reuses the game's per-eye pose and FOV, so when the runtime
does its asynchronous reprojection (motion smoothing on SteamVR), our
passthrough goes along for the ride and stays head-locked at the right
position. The alpha channel of the texture is what gates whether a given pixel
shows passthrough or the game underneath.

## Why head-locked passthrough (and not world-locked)

Without depth estimation, we cannot place camera content correctly in world
space — a "world-locked" cutout would drift visibly as soon as the user moves
their head, because the 2D camera image has no 3D anchor. For the flight-sim
use case the user's hands are essentially stationary relative to their head
(both are in the cockpit, both move together when they lean), so head-locking
the passthrough is acceptable. It's also what Quest's selective passthrough
does in Virtual Desktop when the cutouts are around controllers, modulo a
controller-pose anchor.

## Threading model

* Camera shared-memory producer: dedicated worker thread inside `CameraSource`. Decompresses BC4→R8 and swaps into a front buffer; compositor thread consumes via `try_get_latest`.
* Compositor: single-threaded D3D11 immediate context — only one context is ever used.

## Failure modes and how the layer degrades

| Failure                                    | Behaviour                       |
|--------------------------------------------|---------------------------------|
| Driver shared memory unavailable           | Layer is inert; game unaffected |
| Non-D3D11 graphics binding                 | Layer is inert; game unaffected |
| Game uses no projection layer (rare)       | Layer is inert                  |
| Shared-memory ABI mismatch (driver update) | Read fails, layer goes inert    |

In every failure mode the underlying `xrEndFrame` call is forwarded unmodified
to the next layer. The layer never breaks the game.

## What was deferred

* D3D12 / Vulkan support. Adding D3D12 is straightforward (share a D3D11 device
  via `OpenSharedResource` and blit into a D3D12 swapchain image); Vulkan
  requires a Vulkan-side composite path or a shared texture via `KHR_external_memory`.
* World-locked passthrough using stereo triangulation of the two camera feeds.
  Possible in principle (the cameras have a known baseline), but slow and
  fragile, and the practical win for sim cockpit use is minimal.
* Per-game bind profiles (e.g. per-game passthrough button presets).
