# PSVR2 Passthrough Layer

<img width="1195" height="896" alt="image" src="https://github.com/user-attachments/assets/ae1248dc-d033-43ba-92ae-750a4f4a26ac" />
Meme courtesy of Sgt.Ozone.


An OpenXR implicit API layer that injects real-time stereo passthrough from the PSVR2's
built-in bottom cameras into any OpenXR application running under SteamVR on PC.

<img width="1266" height="636" alt="repository-open-graph-template" src="https://github.com/user-attachments/assets/db3b4f1e-ad0c-4bc6-9feb-77585c79bb57" />

See https://www.youtube.com/watch?v=WyVbhiK8BAc for a demo. 

Intended targets: DCS World, and other D3D11 OpenXR titles. 

**Alpha software — geometry alignment WIP**

Feedback via [github discussion tab](https://github.com/Obsidiate/psvr2passthrough/discussions) above, or the [offical subreddit /r/psvr2passthrough](https://old.reddit.com/r/psvr2passthrough/)

## What it does

- **Camera ingestion** — reads stereo grayscale frames directly from the PSVR2 driver's
   shared-memory interface. No helper process required; the layer talks to the driver directly.
- **Lens undistortion** — applies the per-eye calibration coefficients provided by the driver.
- **Stereo geometry correction** — corrects for the cameras' physical mounting angle
   (toe-out, tilt-down, roll) via a baked rectification mesh. Adjustable per-eye in the
   config GUI; paired (symmetric) adjustment is the default.
- **Button-gated compositing** — intercepts `xrEndFrame` and injects an
   `XrCompositionLayerProjection` per eye. Visibility is controlled by a user-configured
   binding (keyboard key, XInput gamepad button, or DirectInput HOTAS/joystick button) in
   either hold-to-show or toggle mode. Passthrough can also be forced always-on for
   calibration.
- **Quad-views compatible** — tested and primarily iterated in DCS World.
- **OBS recording / mirror layer compatible**
- **OpenKneeboard compatible**

## Known Limitations

- The camera feeds are passed over USB and are a compressed feed at a lower framerate versus the in headset native view, with a resultant drop in quality and potentially higher "VR legs/nausea" effect. Fine for reaching for panels, not for "mixed reality" use.
- There are mathematical aspects to the undistortion model that may not be exposed to the PC in shared memory that are translated directly in the pipeline from camera > in headset passthrough view. Research continues.
- A configuration gui has been provided to tweak some values as a result of minor differences between headsets, such as camera rotation.
- Refinement on theses default values is not completed. Expected iteration of this rapidly in coming weeks.
- ~30-60 Hz camera feed vs 90/120 Hz game rendering — passthrough will lag fast head motion slightly.
- D3D11 host-app graphics only.

## Features

- Button-gated passthrough: keyboard keys, XInput gamepad buttons, DirectInput HOTAS/joystick buttons
- Hold-to-show or toggle mode
- Brightness, opacity, zoom, and stereo geometry calibration sliders.
- Standalone configuration GUI (PSVR2PassthroughConfig.exe)

## What this is NOT

- It does **not** require any helper process beyond SteamVR itself.
- It does **not** support the top two PSVR2 cameras — Sony does not expose them to PC.
- It does **not** modify game files or inject into game processes.

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
- `build/src/layer/Release/PSVR2PassthroughLayer.dll` — the API layer
- `build/src/gui/Release/PSVR2PassthroughConfig.exe` — the configuration GUI

## Install

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

## Configuration

YOU MUST Run `PSVR2PassthroughConfig.exe` to configure the binding to show the layer. The GUI provides:

- Master on/off switch and force-passthrough debug toggle
- Opacity control
- Button binding capture (keyboard, XInput gamepad, DirectInput HOTAS/joystick)
  with hold-to-show or toggle-on/off mode
- Lens undistortion toggle and zoom control
- Per-eye stereo geometry sliders (toe-out, tilt-down, roll) in degrees, with paired or independent adjustment
- Headset intrinsics display (fx, fy, cx, cy read from the PSVR2 driver)

Settings are saved to `%LOCALAPPDATA%\PSVR2PassthroughLayer\config.json` and
take effect on the next sim launch.

## Runtime requirements

- **PSVR2 PC adapter** (official Sony) — for connecting the headset
- **SteamVR** — provides the OpenXR runtime; also activates the camera feed

Start SteamVR with the headset connected, then launch your sim normally. The
layer activates automatically.

## Log file

`%LOCALAPPDATA%\PSVR2PassthroughLayer\layer.log` — truncated on each launch.

## Attributions

- **[PSVR2Camera](https://github.com/realSupremium/PSVR2Camera)** by realSupremium — shared memory layout, IPC object naming, and calibration data offsets for the PSVR2 driver interface. This project would not exist without that reverse-engineering work.
- **[OpenKneeboard](https://github.com/OpenKneeboard/OpenKneeboard)** by Fred Emmott — reference for DirectInput device enumeration and background (non-exclusive) binding in an OpenXR layer context.
- **[Dear ImGui](https://github.com/ocornut/imgui)** by Omar Cornut — immediate-mode GUI library used for the configuration app.
- **[spdlog](https://github.com/gabime/spdlog)** / **[fmt](https://github.com/fmtlib/fmt)** — logging and string formatting.
- **[Khronos OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK)** — OpenXR loader and headers.

## Licence

MIT.
