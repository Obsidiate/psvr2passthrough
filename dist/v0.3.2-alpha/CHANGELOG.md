# Changelog

### This is not expected to be perfect 1:1 of in headset passthrough at this Alpha stage but it is veeeeery usable. 

## v0.3.2-alpha

**Experimental reprojection (off by default)**

Very early, placeholder work toward reducing the motion lag visible in the passthrough image. The layer now intercepts `xrBeginSession` and `xrWaitFrame` to establish a clock calibration offset between steady_clock and SteamVR's XrTime domain, extracts the SLAM-corrected head pose from the driver shared memory at each camera frame, and calls `xrLocateViews` with a historically-measured capture timestamp so the compositor's ATW can warp the passthrough image from actual capture time to scanout time rather than from the game's predicted display time.

In testing, this does not yet eliminate the most visible motion artefact. The shimmering double-edge effect on fast head pivots (symmetric ghost on both the leading and trailing edge of objects) is a consequence of the 60fps camera running on a 120Hz display - each camera frame must be used for two consecutive display frames, which ATW renders at slightly different orientations. The likely fix is frame interpolation between consecutive camera frames, but this carries a potentially prohibitive per-frame GPU cost and has not yet been attempted.

Reprojection is disabled by default. It can be enabled in the configuration GUI under the new Reprojection section, or by setting `"reprojection_enabled": true` in `config.json`. Treat as a preview only.

&nbsp;

**Plain English:** Super early attempt at making the camera image track your head movement better. It mostly doesn't work yet - there is a known shimmering/ghosting effect on fast movement that we have not solved. Off by default. You can turn it on to see what it does, but do not expect much.

&nbsp;

**Shared memory CPU usage fix**

The PSVR2 driver image event is a manual-reset event that remains permanently signaled once the first camera frame has arrived. Without tracking which frame was last copied, the camera thread would sometimes spin re-reading the same stale frame at tens of thousands of iterations per second, causing noticeable single-thread CPU spikes. A per-slot timestamp comparison now prevents redundant copies.

**GUI: update checker**

The configuration tool now checks for new releases at startup and shows a status banner - amber if a newer version is available with a link to the releases page, green if you are up to date. No data beyond a single HTTPS request to the GitHub releases API is sent.

**GUI: version and feedback links**

The About panel now shows the current version string, with direct links to GitHub Discussions and the community subreddit.

---

## v0.3.1-alpha

**BC4 decompression moved to GPU**
Camera frames are now uploaded as raw `DXGI_FORMAT_BC4_UNORM` block-compressed data and decompressed entirely by the GPU hardware during texture sampling. The previous per-frame software decode loop (524,288 bytes per eye, every frame) has been removed. This reduces CPU load on the camera thread, eliminates any timing coupling between camera frame delivery and decompression, and has produced a noticeable improvement in perceived latency. As a rough estimate, this change reduces overall CPU usage by around 2-5%, and cuts the memory bandwidth consumed by frame processing by more than half - a modest but real benefit on CPU-bound sim rigs.

&nbsp;

**Plain English:** Moved some brain things from CPU to GPU. Picture comes through faster. Less vom vom.

&nbsp;

Thanks to the community at the **PSVR2Toolkit Discord** for the nudge in this direction - a contributor who preferred not to be named specifically raised the idea. Much appreciated. And another shoutout to the **psvr2camera** project for making all of this possible.

---

## v0.3-alpha

**Massive visual quality improvement**
Contrast control, unsharp masking, and a gamma pipeline correction combine to produce a significantly sharper, more natural-looking passthrough image — closer to the native PSVR2 passthrough experience than any previous release.

**Image quality controls**
Three new per-pixel adjustments are now applied in the shader pipeline: a **contrast** multiplier that lifts the tonal range to compensate for the PSVR2 driver's auto-exposure flattening, and an **unsharp mask** (strength + radius) that recovers edge detail lost to BC4 compression. Both are individually toggleable — contrast has its own enable checkbox; unsharp masking has a section-level toggle. Neutral values are used when disabled so the pipeline remains a single shader pass.

**Improved configuration GUI**
The left and right panels are now independently scrollable. All hint text wraps to the available column width. Ctrl+Click on any slider allows typing an exact value (noted in the Tips panel). Specific sim names removed from the UI. A stereo geometry calibration notice prompts community feedback on default values.

**Gamma pipeline correction**
The compositor eye-target texture is now correctly `R8G8B8A8_UNORM` (no sRGB encoding on write). CopyResource transfers raw bits to the `UNORM_SRGB` swapchain, letting SteamVR interpret the camera's native gamma-encoded values correctly. Previously this pipeline was inverted, causing a washed-out, low-contrast image.

**BC4 decompression accuracy**
Round-to-nearest correction applied to the software BC4 decoder (`+3` before `/7` and `+2` before `/5` per spec). Eliminates a subtle systematic bias in decompressed pixel values.

**Zero-copy camera frame handoff**
The camera producer thread now swaps buffers with the render thread rather than copying, eliminating a per-frame 2MB allocation. A consumed-frame flag prevents re-rendering stale frames when the game renders faster than the camera.

**Codebase cleanup**
Removed dead detection, hand-tracking, and tools code that was no longer referenced. Stale CMakeLists post-build steps, dead API surface, and orphaned static locals replaced with member variables for correct behaviour on session recreation.

---

## v0.2-alpha

**Per-eye geometry calibration**
Toe-out, tilt and roll are now configured independently per eye. Default mode pairs them
symmetrically (adjusting one mirrors the other), with an "Unlock eyes" button for independent
left/right tuning. This allows finer correction for potential physical variance between
individual headsets. Methods to automate this calibration are being explored for a future release.

**Geometry controls in degrees**
The toe-out, tilt and roll sliders now display and accept values in degrees rather than radians.

**Headset intrinsics panel**
The configuration GUI right column shows the active fx, fy, cx, cy values read from the PSVR2
driver at the last session start, confirming that per-headset factory calibration is in use.

**Calibration dump**
Each session writes `%LOCALAPPDATA%\PSVR2PassthroughLayer\calibration_dump.txt` containing
the full factory intrinsics and 20-coefficient distortion polynomial for both eyes.

---

## v0.1-alpha

Initial release.
