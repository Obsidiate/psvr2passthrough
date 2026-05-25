# Design notes

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

## Why we don't use XR_EXT_hand_tracking

Two reasons. First, exposing hand-tracking through the runtime would mean
games consume it — which is *not* the goal (you have HOTAS / yoke / stick).
Second, even if we wanted to, the lower PSVR2 cameras don't provide enough
information for high-quality skeletal estimation, and trying to do so would
add latency we can't afford.

The detector here outputs only "where in the image" — 2D bounding boxes
per eye. That's enough to draw a circular mask. We deliberately don't even
stereo-rectify to recover depth; the per-eye 2D position lines up with the
per-eye camera image naturally, which is the only thing we need for a 2D
mask in image space.

## Threading model

* Camera shared-memory producer: dedicated worker thread inside CameraSource.
* Hand detection: runs synchronously on the OpenXR render thread inside
  `xrEndFrame`. At 192×192 input the lite palm detector runs in well under
  the budget per frame on a 2020-era CPU; if profiling shows otherwise, the
  detection can be moved to its own thread feeding a SPSC most-recent-result
  slot. The compositor is single-threaded D3D11; only one immediate context is
  ever used.
* Recenter action: polled in `xrSyncActions` on the render thread.

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
* GUI configurator. Right now config is hard-coded defaults; the natural next
  step is a small tray-application reading/writing `%LOCALAPPDATA%\PSVR2PassthroughLayer\config.json`
  with the `CompositorConfig` fields.
* Per-game bind toggles. The recenter binding is implicit; per-game profiles
  (e.g. "use DCS's recenter binding") would require either OpenXR Toolkit-style
  per-game configs or just letting the user rebind in SteamVR.
