# PSVR2 Passthrough Layer

**Alpha software — geometry alignment requires manual calibration per headset. See Configuration below.**

An OpenXR implicit API layer that injects real-time stereo passthrough from the PSVR2's
built-in bottom cameras into any OpenXR application running under SteamVR on PC.

Intended targets: DCS World, MSFS 2024, iRacing, and any other SteamVR OpenXR title.

## What it does

1. **Camera ingestion** — reads stereo grayscale frames directly from the PSVR2 driver's
   shared-memory interface. No helper process required; the layer talks to the driver directly.
2. **Lens undistortion** — applies the per-eye calibration coefficients provided by the driver.
3. **Stereo geometry correction** — corrects for the cameras' physical mounting angle
   (toe-out, tilt-down, roll) via a baked rectification mesh. Adjustable per-eye in the
   config GUI; paired (symmetric) adjustment is the default.
4. **Button-gated compositing** — intercepts `xrEndFrame` and injects an
   `XrCompositionLayerProjection` per eye. Visibility is controlled by a user-configured
   binding (keyboard key, XInput gamepad button, or DirectInput HOTAS/joystick button) in
   either hold-to-show or toggle mode. Passthrough can also be forced always-on for
   calibration.

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

```powershell
.\scripts\install_layer.ps1   # run as admin
```

Registers the layer under
`HKLM\Software\Khronos\OpenXR\1\ApiLayers\Implicit` so it loads automatically
for every OpenXR application. To uninstall:

```powershell
.\scripts\uninstall_layer.ps1
```

## Configuration

Run `PSVR2PassthroughConfig.exe` to configure the layer. The GUI provides:

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

## Known limitations

- 60 Hz camera feed vs 90/120 Hz game rendering — passthrough will lag fast head
  motion slightly.
- Lower cameras only — roughly sternum-height and below.
- Grayscale output.
- D3D11 host-app graphics only. D3D12/Vulkan apps see the layer go inert.

## Licence

MIT.
