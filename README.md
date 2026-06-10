# PSVR2 Passthrough Layer

Real-time stereo passthrough from the PSVR2's built-in bottom cameras into your
SteamVR games on PC. Ships in two forms that share one engine:

- an **OpenXR API layer** that injects passthrough into OpenXR games (DCS, MSFS), and
- an **OpenVR overlay app** that composites passthrough over *any* SteamVR game —
  **including native-OpenVR titles like Half-Life: Alyx** that an OpenXR layer can
  never attach to. No OpenComposite required.

<img width="1266" height="636" alt="repository-open-graph-template" src="https://github.com/user-attachments/assets/db3b4f1e-ad0c-4bc6-9feb-77585c79bb57" />

## Wait...What? What does this do?
The image above shows it : Blend the real world with your game using the headsets cameras.  

Watch https://www.youtube.com/watch?v=WyVbhiK8BAc for a live demo. 

## Features
- Button-gated passthrough: keyboard keys, XInput gamepad buttons, DirectInput HOTAS/joystick buttons
- Hold-to-show or toggle mode
- Brightness, opacity, and various other image processing/geometry configuration sliders. 
- Standalone configuration GUI (PSVR2PassthroughConfig.exe).

## Intended targets 
DCS World, MSFS 2024, and other D3D11 / D3D12 OpenXR titles. 

## Usage
Unzip to preferred folder and run install.bat, bind a trigger key. Save config. 
One install, set and forget. No need to run a program every time. Installed layer loads with the game. 

**Alpha software: Rapidly being iterated. Check back often.**

Feedback via [github discussion tab](https://github.com/Obsidiate/psvr2passthrough/discussions) above, or the [offical subreddit /r/psvr2passthrough](https://old.reddit.com/r/psvr2passthrough/)

## All PSVR2 users should also check out 
 [PSVR2toolkit](https://github.com/BnuuySolutions/PSVR2Toolkit) for enabling eye tracking   

 [QuadViews](https://github.com/mbucchia/Quad-Views-Foveated) to enable it in DCS  
 
 and [Quadviews Companion](https://github.com/TallyMouse/QuadViewsCompanion) to tweak the foveated rendering values. Amazing projects.  
 

## What it does - More detail.

- **Camera ingestion:** Reads stereo grayscale frames directly from the PSVR2 driver's
   shared-memory interface. No helper process required.
- **Lens undistortion:** applies the per-eye calibration coefficients provided by the driver.
- **Stereo geometry correction:** corrects for the cameras' physical mounting angle
   (toe-out, tilt-down, roll) via a baked rectification mesh. Adjustable per-eye in the
   config GUI; paired (symmetric) adjustment is the default.
- **Button-gated compositing:** intercepts `xrEndFrame` and injects an
   `XrCompositionLayerProjection` per eye. Visibility is controlled by a user-configured
   binding (keyboard key, XInput gamepad button, or DirectInput HOTAS/joystick button) in
   either hold-to-show or toggle mode. Passthrough can also be forced always-on for
   calibration.
- **Quad-views compatible:** tested and primarily iterated in DCS World.
- **OBS recording / mirror layer compatible**
- **OpenKneeboard compatible**

## Architecture

Both front-ends share one runtime-agnostic engine (camera ingestion, calibration,
undistortion/rectification mesh, and the D3D11 render pass). Only the final
submission differs: the layer hooks `xrEndFrame`; the overlay submits an OpenVR
overlay texture.

```
                 PSVR2 driver (SteamVR)
                 shared memory + event + mutex
                            │
                psvr2_passthrough_core
        (camera ingest · calibration · FOV/IPD math ·
         undistortion mesh · D3D11 render pass)
                    ┌───────┴───────┐
                    ▼               ▼
        OpenXR API layer      OpenVR overlay app
        (PSVR2Passthrough-    (PSVR2Passthrough-
         Layer.dll,            Overlay.exe,
         xrEndFrame hook)      IVROverlay texture,
                               head-locked)
                    └───────┬───────┘
                            ▼
                   SteamVR compositor
                            ▼
                      PSVR2 headset
```

- **OpenXR layer** — zero extra process; loads into OpenXR games (D3D11/D3D12).
- **OpenVR overlay** — a background app SteamVR composites over every game,
  regardless of graphics API, so it reaches native-OpenVR titles too.

## Working / not-working titles

Pick the front-end that matches the game:

- **OpenXR games** (D3D11/D3D12) → the **OpenXR layer** loads automatically.
- **Native-OpenVR games** (no OpenXR loader) → run the **OpenVR overlay app**.
  This is the recommended path for Alyx, No Man's Sky, etc. — no OpenComposite needed.

**Known working — OpenXR layer**

| Title | Graphics API | Notes |
|-------|--------------|-------|
| DCS World | D3D11 | Primary development and target title. Quad-views tested. |
| Microsoft Flight Simulator 2024 | D3D12 | Works in DX12 mode via D3D11-on-12 interop. Expect the usual GPU-bound framerate cost at high graphics settings. |

**Known working — OpenVR overlay**

| Title | Why |
|-------|-----|
| Half-Life: Alyx | Native OpenVR title. The overlay composites over it via SteamVR, so passthrough works without OpenComposite. |
| No Man's Sky | Native OpenVR. Same — covered by the overlay. |
| (any SteamVR game) | The overlay sits at the SteamVR compositor level, so it is API-agnostic. |

**Why the overlay reaches games the layer can't**

Some VR games render through Valve's older OpenVR API instead of OpenXR. An OpenXR
API layer only loads into games that use the OpenXR loader, so it never attaches to a
native-OpenVR game. The **OpenVR overlay app** sidesteps this entirely: it is a
standalone SteamVR overlay that the compositor draws over whatever game is running,
regardless of which graphics/VR API that game uses.

> Note: overlay passthrough is **full-FOV, opaque, head-locked, toggleable room
> visibility** — it does not interact with game content. (OpenComposite is still an
> option if you specifically want an OpenVR game to run *through the OpenXR layer*,
> but it is not required for passthrough.)

## Limitations

- The camera feeds are passed over USB and are a compressed feed at a lower framerate versus the in headset native view, with a resultant drop in quality and potentially higher "VR legs/nausea" effect. Fine for reaching for panels, not for "mixed reality" use.
- There are mathematical aspects to the undistortion model that may not be exposed to the PC in shared memory that are translated directly in the pipeline from camera > in headset passthrough view. Research continues.
- A configuration gui has been provided to tweak some values as a result of minor differences between headsets, such as camera rotation.
- Refinement on theses default values is not completed. Expected iteration of this rapidly in coming weeks.
- ~30-60 Hz camera feed vs 90/120 Hz game rendering. Passthrough will lag fast head motion slightly.

## What this is NOT
- The **OpenXR layer** does **not** require any helper process beyond SteamVR itself.
  The **OpenVR overlay** is itself a small background process (it autostarts with
  SteamVR once registered).
- It does **not** support the top two PSVR2 cameras. Sony does not expose them to PC.

## Build (skip this if downloading a release package)

Requirements:
- Windows 10/11, Visual Studio 2022 (MSVC v143)
- CMake ≥ 3.24
- vcpkg (bootstrapped, with `VCPKG_ROOT` env var set)
- SteamVR with the PSVR2 PC adapter

```powershell
git clone --recursive <this-repo>
cd psvr2_passthrough_layer
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

Output:
- `build/src/layer/Release/PSVR2PassthroughLayer.dll` : the OpenXR API layer
- `build/src/overlay/Release/PSVR2PassthroughOverlay.exe` : the OpenVR overlay app
- `build/src/gui/Release/PSVR2PassthroughConfig.exe` : the configuration GUI

## Install

### OpenXR layer (OpenXR games — DCS, MSFS)

unzip release to installation folder and use the install.bat, or 

from a powershell prompt, run 
```powershell
install_layer.ps1   # run as admin
```

Registers the layer under
`HKLM\Software\Khronos\OpenXR\1\ApiLayers\Implicit` so it loads automatically
for every OpenXR application. 

To uninstall:

```powershell
uninstall_layer.ps1
```

### OpenVR overlay (native-OpenVR games — Alyx, No Man's Sky, …)

Register the overlay with SteamVR once; it then autostarts with SteamVR:

```powershell
PSVR2PassthroughOverlay.exe --register
```

To stop it autostarting and remove it from SteamVR:

```powershell
PSVR2PassthroughOverlay.exe --unregister
```

`--register` installs `manifest.vrmanifest` (shipped next to the exe) via SteamVR's
application registry and enables autolaunch. Toggle passthrough with your configured
button binding, or with the default SteamVR binding (right Sense **B**, long-press),
which you can remap in SteamVR's controller-binding UI.

## Configuration

YOU MUST Run `PSVR2PassthroughConfig.exe` to configure the binding to show the layer. The GUI provides:

- Master on/off switch and force-passthrough debug toggle
- Opacity control
- Button binding capture (keyboard, XInput gamepad, DirectInput HOTAS/joystick)
  with hold-to-show or toggle-on/off mode
- Lens undistortion toggle and zoom control
- Per-eye stereo geometry sliders (toe-out, tilt-down, roll) in degrees, with paired or independent adjustment
- OpenVR overlay settings (virtual distance, opacity) for native-OpenVR games
- Headset intrinsics display (fx, fy, cx, cy read from the PSVR2 driver)

Settings are saved to `%LOCALAPPDATA%\PSVR2PassthroughLayer\config.json` and are
shared by **both** front-ends — the OpenXR layer reads them on the next sim launch,
and the OpenVR overlay reads them on launch.

## Runtime requirements

- **PSVR2 PC adapter** (official Sony): for connecting the headset
- **SteamVR**: provides the OpenXR runtime; also activates the camera feed

Start SteamVR with the headset connected, then launch your sim normally. The
layer activates automatically.

## Log files

Both truncated on each launch, in `%LOCALAPPDATA%\PSVR2PassthroughLayer\`:

- `layer.log` : the OpenXR layer
- `overlay.log` : the OpenVR overlay app

## Attributions

- **[PSVR2Camera](https://github.com/realSupremium/PSVR2Camera)** by realSupremium: shared memory layout, IPC object naming, and calibration data offsets for the PSVR2 driver interface. This project would not exist without that reverse-engineering work.
- **[OpenKneeboard](https://github.com/OpenKneeboard/OpenKneeboard)** by Fred Emmott: reference for DirectInput device enumeration and background (non-exclusive) binding in an OpenXR layer context.
- **[Dear ImGui](https://github.com/ocornut/imgui)** by Omar Cornut: immediate-mode GUI library used for the configuration app.
- **[spdlog](https://github.com/gabime/spdlog)** / **[fmt](https://github.com/fmtlib/fmt)**: logging and string formatting.
- **[Khronos OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK)**: OpenXR loader and headers.
- **[OpenVR SDK](https://github.com/ValveSoftware/openvr)** by Valve: overlay, input, and application APIs used by the OpenVR overlay app.

## Licence

MIT.

<img width="1195" height="896" alt="image" src="https://github.com/user-attachments/assets/ae1248dc-d033-43ba-92ae-750a4f4a26ac" />
Meme courtesy of Sgt.Ozone.
