"""
label_assist.py — MoveNet-based pre-annotation generator for PSVR2 wrist labelling.

Runs MoveNet Lightning on collected PNG frames, writes a Label Studio pre-annotation
JSON that can be imported as predictions, reducing manual labelling to corrections.

Usage:
    python label_assist.py --frames C:/recordings --output preannotations.json

Requirements:
    pip install tensorflow tensorflow-hub opencv-python numpy

MoveNet Lightning is used (not Thunder) because it is faster and sufficient for
coarse wrist detection; the human labeller corrects any errors.

Keypoint index mapping (MoveNet):
    9  = left_wrist
    10 = right_wrist

Confidence threshold:
    Predictions below LOW_CONF_THRESHOLD are included but flagged for careful review.
    Predictions below SKIP_THRESHOLD are omitted entirely (not worth correcting from).
"""

import argparse
import glob
import json
import os
import sys

import cv2
import numpy as np

LOW_CONF_THRESHOLD  = 0.40   # flagged for careful manual review
SKIP_THRESHOLD      = 0.10   # below this: omit prediction, leave unlabelled

MOVENET_LEFT_WRIST  = 9
MOVENET_RIGHT_WRIST = 10


def load_movenet():
    try:
        import tensorflow as tf
        import tensorflow_hub as hub
    except ImportError:
        sys.exit("Install TensorFlow and tensorflow-hub:  pip install tensorflow tensorflow-hub")

    print("Loading MoveNet Lightning...")
    model = hub.load("https://tfhub.dev/google/movenet/singlepose/lightning/4")
    return model.signatures["serving_default"], tf


def run_movenet(infer_fn, tf_module, image_bgr):
    img_rgb = cv2.cvtColor(image_bgr, cv2.COLOR_BGR2RGB)
    # MoveNet Lightning expects 192×192; input frames are 1024×1024.
    # We resize here — no quality loss for wrist detection at this scale.
    resized = cv2.resize(img_rgb, (192, 192))
    tensor  = tf_module.cast(tf_module.expand_dims(resized, axis=0), dtype=tf_module.int32)
    outputs = infer_fn(input=tensor)
    # keypoints_with_scores: [1, 1, 17, 3] — (y, x, score) normalised to [0, 1]
    kps = outputs["output_0"].numpy()[0, 0]  # shape (17, 3)
    return kps


def load_frame_metadata(frames_dir: str) -> dict:
    """Returns {stem: metadata_dict} for all JSON files in frames_dir."""
    meta = {}
    for jf in glob.glob(os.path.join(frames_dir, "*.json")):
        stem = os.path.splitext(os.path.basename(jf))[0]
        try:
            with open(jf) as f:
                meta[stem] = json.load(f)
        except Exception:
            pass
    return meta


def build_prediction(image_path: str, kps: np.ndarray, metadata: dict | None) -> dict:
    """Build one Label Studio prediction object for a single frame."""
    fname = os.path.basename(image_path)
    results = []
    review_flags = []

    def add_kp(label: str, kp_idx: int):
        y_norm, x_norm, score = float(kps[kp_idx, 0]), float(kps[kp_idx, 1]), float(kps[kp_idx, 2])
        if score < SKIP_THRESHOLD:
            return
        flag = score < LOW_CONF_THRESHOLD
        if flag:
            review_flags.append(f"{label} low_conf={score:.2f}")
        results.append({
            "from_name": "keypoints",
            "to_name":   "image",
            "type":      "keypointlabels",
            "score":     score,
            "value": {
                "x":      x_norm * 100.0,   # Label Studio uses 0-100 percentage
                "y":      y_norm * 100.0,
                "width":  1.0,
                "keypointlabels": [label],
            },
        })

    add_kp("LeftWrist",  MOVENET_LEFT_WRIST)
    add_kp("RightWrist", MOVENET_RIGHT_WRIST)

    # Propagate motion flag from frame metadata
    motion_blur = False
    if metadata:
        motion_blur = metadata.get("motion_flagged", False)
        if not motion_blur:
            ang_vel = metadata.get("hmd_angular_velocity", 0.0)
            motion_blur = ang_vel > 0.8

    if motion_blur:
        review_flags.append("motion_blur")

    prediction = {
        "file_upload": fname,
        "predictions": [{
            "model_version": "movenet_lightning_v4",
            "score":         float(np.mean([r["score"] for r in results])) if results else 0.0,
            "result":        results,
        }],
        "meta": {
            "needs_review":    bool(review_flags),
            "review_reasons":  review_flags,
            "motion_flagged":  motion_blur,
        },
    }
    return prediction


def main():
    parser = argparse.ArgumentParser(description="Generate Label Studio pre-annotations using MoveNet")
    parser.add_argument("--frames",  required=True, help="Directory containing _L.png / _R.png frames")
    parser.add_argument("--output",  default="preannotations.json", help="Output JSON path")
    parser.add_argument("--eye",     choices=["L", "R", "both"], default="both",
                        help="Which eye frames to process (default: both)")
    args = parser.parse_args()

    infer_fn, tf_module = load_movenet()
    metadata_map = load_frame_metadata(args.frames)

    suffixes = []
    if args.eye in ("L", "both"): suffixes.append("_L")
    if args.eye in ("R", "both"): suffixes.append("_R")

    all_pngs = sorted(glob.glob(os.path.join(args.frames, "*.png")))
    target_pngs = [p for p in all_pngs if any(os.path.basename(p).endswith(s + ".png") for s in suffixes)]

    if not target_pngs:
        sys.exit(f"No PNG frames found in {args.frames} matching eye={args.eye}")

    print(f"Processing {len(target_pngs)} frames...")

    predictions = []
    needs_review_count = 0

    for i, png_path in enumerate(target_pngs):
        fname  = os.path.basename(png_path)
        # Derive metadata key: strip _L or _R suffix and .png
        stem   = fname.rsplit("_", 1)[0]   # e.g. "1234567890_00000042"
        meta   = metadata_map.get(stem)

        img = cv2.imread(png_path, cv2.IMREAD_GRAYSCALE)
        if img is None:
            print(f"  [WARN] Cannot read {fname}, skipping")
            continue

        # MoveNet expects RGB; replicate single channel to 3
        img_bgr = cv2.cvtColor(img, cv2.COLOR_GRAY2BGR)
        kps     = run_movenet(infer_fn, tf_module, img_bgr)
        pred    = build_prediction(png_path, kps, meta)
        predictions.append(pred)

        if pred["meta"]["needs_review"]:
            needs_review_count += 1
            print(f"  [REVIEW] {fname}  reasons={pred['meta']['review_reasons']}")

        if (i + 1) % 50 == 0:
            print(f"  {i+1}/{len(target_pngs)} done")

    with open(args.output, "w") as f:
        json.dump(predictions, f, indent=2)

    print(f"\nDone. {len(predictions)} predictions written to {args.output}")
    print(f"Flagged for careful review: {needs_review_count}/{len(predictions)}")
    print("\nImport into Label Studio:")
    print("  Project → Import → select JSON → choose this file")
    print("  Label Studio will load predictions as proposal annotations.")


if __name__ == "__main__":
    main()
