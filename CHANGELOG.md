# Changelog

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
