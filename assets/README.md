# Model assets

The hand detector expects `palm_detection_lite.onnx` here at runtime.

Mediapipe's palm detector is shipped as a `.tflite` file. To produce the
ONNX version this project uses:

```powershell
# Once, in a Python venv:
pip install tf2onnx tensorflow

# Convert (palm_detection_lite.tflite available from
# https://storage.googleapis.com/mediapipe-assets/palm_detection_lite.tflite):
python -m tf2onnx.convert `
    --tflite palm_detection_lite.tflite `
    --output palm_detection_lite.onnx `
    --opset 13
```

Drop the resulting `palm_detection_lite.onnx` in this folder before building,
or copy it next to the built DLL after building. The CMake post-build step
mirrors the `assets/` tree next to the DLL automatically.

If the model is absent the layer will still load but will draw no cutouts.

## Licence

MediaPipe is Apache 2.0, see https://github.com/google-ai-edge/mediapipe
