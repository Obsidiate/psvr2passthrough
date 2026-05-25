# Using the layer alongside Quad-Views-Foveated (DCS World)

[Quad-Views-Foveated](https://github.com/mbucchia/Quad-Views-Foveated) is
mbucchia's API layer that emulates the `XR_VARJO_quad_views` extension on
headsets that don't natively support it. It is the standard way to get
foveated rendering working in DCS World on PSVR2, and it composes the four
projection views (two peripheral + two focus) back down to two stereo views
before they reach the OpenXR runtime.

## TL;DR

The PSVR2 Passthrough Layer is compatible with Quad-Views-Foveated. The
combined stack is known to work in DCS as of late 2025. There is one detail
you have to get right: **layer ordering**.

## The ordering rule

OpenXR API layers form a chain. The application sits at the top; the runtime
(SteamVR's OpenXR for PSVR2) sits at the bottom. Layers in between each see
the frame submission as it travels down the chain.

For our layer to work properly with Quad-Views-Foveated, the chain must look
like this (top → bottom):

```
DCS World
   │   submits XrCompositionLayerProjection with viewCount = 4
   ▼
Quad-Views-Foveated
   │   composes the 4 quad-view projections into 2 stereo views
   ▼
PSVR2 Passthrough Layer        ← we see viewCount == 2 here
   │   appends our passthrough projection on top
   ▼
SteamVR OpenXR runtime
   ▼
PSVR2 headset
```

The Passthrough Layer also explicitly handles the case where it sees
`viewCount == 4` directly (i.e. if Quad-Views-Foveated runs *after* us in the
chain, or isn't installed at all). In that scenario it uses views 0 and 1, the
peripheral full-FOV views — those are the ones that always cover the
cockpit area where your hands actually are.

So in practice the layer works in any ordering, but the ordering shown above
gives the highest-quality result because the passthrough quality follows the
final composed image rather than the higher-resolution focus inset.

## Adjusting layer order

Use [OpenXR-API-Layers-GUI](https://github.com/fredemmott/OpenXR-API-Layers-GUI)
(by fredemmott — the same person behind XR Compositor for Steam Link). Launch
it, find the entries for `Quad-Views-Foveated` and `PSVR2 Passthrough Layer`,
and drag them so the Quad-Views entry appears *above* the Passthrough entry
in the list. Save.

If you don't have OpenXR-API-Layers-GUI installed, the layers are usually
listed in order of registration time anyway, so installing Quad-Views-Foveated
*before* this layer typically gives the right order without any manual fiddling.

## DCS-specific setup notes

If you're new to the DCS + Quad-Views-Foveated stack, the [project wiki](https://github.com/mbucchia/Quad-Views-Foveated/wiki)
is the authoritative source. The minimum prerequisites:

- DCS must run in **multi-threaded mode** (`dcs_mt.exe`, or set "Play MT
  Preview" launch options in Steam).
- Inside DCS: disable **Bloom** (under VR) and **Lens Effects** (under System).
- If you previously used OpenXR Toolkit, some of its settings can conflict
  with Quad-Views-Foveated. Reset Toolkit to defaults before troubleshooting.

These rules are inherited from Quad-Views-Foveated — the Passthrough Layer
adds no new DCS-specific requirements.

## Foveation caveat

Quad-Views-Foveated renders the *focus* region at a higher pixel density than
the *peripheral* region. Our passthrough composites onto the peripheral
(full-FOV) views — so if your detected hands happen to fall inside the
foveated focus region, the cutout edge will look slightly softer there than
elsewhere. In practice the focus region is centred near gaze, and you don't
usually stare at your HOTAS hands, so this rarely matters. If you do notice
it, increase the cutout feather a bit to make the transition invisible.
