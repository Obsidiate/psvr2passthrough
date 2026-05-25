# PSVR2 Passthrough Layer

**_### I EXPECT 0.1 TO BE SLIGHTLY VISUALLY BROKEN FOR YOU, THIS IS AN ALPHA PROOF OF CONCEPT_**

An OpenXR implicit API layer that injects real-time stereo passthrough from the PSVR2's
built-in bottom cameras into any OpenXR application running under SteamVR on PC.

Intended targets: DCS World, and other OpenXR compatible titles. 

## What it does

1. **Camera ingestion** — reads stereo grayscale frames directly from the PSVR2 driver's
   shared-memory interface. No helper process required; the layer talks to the driver directly.
2. **Lens undistortion** — applies the per-eye calibration coefficients provided by the driver.
3. **Stereo geometry correction** — corrects for the cameras' physical mounting angle
   (toe-out, tilt-down, roll) via a baked rectification mesh. Pre-tuned to match the native
   PSVR2 passthrough alignment; all three values are live-adjustable in the config GUI.
4. **Button-gated compositing** — intercepts `xrEndFrame` and injects an
   `XrCompositionLayerProjection` per eye. Visibility is controlled by a user-configured
   binding (keyboard key, XInput gamepad button, or DirectInput HOTAS/joystick button) in
   either hold-to-show or toggle mode. Passthrough can also be forced always-on for
   calibration.
   TL:DR : Press (or hold) a button to see the world around you (with adjustable transparency), only in OpenXR VR games.
5. **Quad-views compatible** - Tested and primarily iterated in DCS. Your feedback in other games welcome. 
6. **OBS recording / mirror layer compatible**
7. **OpenKneeBoard Compatible**

## What this is NOT

- It does **not** require any helper process beyond SteamVR itself.
- It does **not** support the top two PSVR2 cameras — Sony does not expose them to PC.
- It does **not** modify game files or inject into game processes.

## Known limitations
- The passthrough camera feeds are passed over USB and are a compressed feed at a lower framerate versus the in headset native view, with a resultant drop in quality and potentially higher "VR legs/nausea" effect. Fine for reaching for panels, not so much for "mixed reality" use at this stage. 
- There are mathematical aspects to the undistortion model that may not be exposed to the PC in shared memory that are translated directly in the pipeline from camera > in headset passthrough view. Research continues.
- A configuration gui has been provided to tweak some values as a result of minor differences between headsets, such as camera rotation. 
- Refinement on theses default values is not completed. Expected iteration of this rapidly in coming weeks. It may feel slightly "off" in a hard to articulate way compared to native passthrough. 
- There is currently no built in version check/update prompt.

## Build (skip this if downloading 0.1 release)

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
- Stereo geometry sliders (toe-out, tilt-down, roll)

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
