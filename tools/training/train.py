"""
train.py — PSVR2 bespoke wrist detection model training pipeline.

Model:  MobileNetV2 alpha=0.35 backbone (pretrained ImageNet, 1-channel modified)
        → GlobalAveragePooling → Dense(64, relu) → Dense(6)
Output: [lx, ly, lconf, rx, ry, rconf]  all in [0, 1]

Usage:
    python train.py --annotations annotations.json --images C:/recordings

Colab setup (run once at top of notebook):
    !pip install torch torchvision onnx onnxruntime
    !pip install timm  # only needed if using timm backbone variant
    from google.colab import drive; drive.mount('/content/drive')
    # Then adjust Config.images_dir and Config.checkpoint_dir to /content/drive/...

Export:  best checkpoint is automatically exported to ONNX after training.
Verify:  onnxruntime inference pass is run on export to confirm shape and latency.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import DataLoader, Dataset
from torchvision.models import MobileNet_V2_Weights, mobilenet_v2
from PIL import Image, ImageEnhance


# ─── Configuration ────────────────────────────────────────────────────────────

@dataclass
class Config:
    # Paths
    annotations_json: str  = "annotations.json"   # Label Studio JSON export
    images_dir:       str  = "recordings"          # directory of PNG frames
    checkpoint_dir:   str  = "checkpoints"

    # Input
    input_size:       int  = 192

    # Training schedule
    batch_size:       int   = 32
    epochs:           int   = 60
    frozen_epochs:    int   = 10   # freeze backbone for first N epochs
    lr_head:          float = 1e-3
    lr_backbone:      float = 1e-4  # used after unfreezing
    weight_decay:     float = 1e-4

    # Loss weighting
    confidence_loss_weight: float = 0.3   # weight on BCE confidence terms
    # position MSE weight is implicitly 1.0

    # Data
    val_split:        float = 0.15  # fraction of data held out for validation
    num_workers:      int   = 4

    # Augmentation
    brightness_jitter: float = 0.20   # ± fraction
    contrast_jitter:   float = 0.20
    crop_zoom_jitter:  float = 0.10   # ± fraction of image size

    # ONNX export
    onnx_opset:        int   = 12
    onnx_output:       str   = "wrist_detector.onnx"


# ─── Dataset ──────────────────────────────────────────────────────────────────

@dataclass
class Sample:
    image_path:  str
    lx: float; ly: float; lconf: float
    rx: float; ry: float; rconf: float


def load_annotations(json_path: str, images_dir: str) -> list[Sample]:
    """
    Parses Label Studio JSON export into Sample list.

    Label Studio keypoint x/y are in 0-100 percentage space.
    Absent keypoints (not labelled) → coord 0,0 with conf 0.
    """
    with open(json_path) as f:
        data = json.load(f)

    samples = []
    for task in data:
        # Resolve image path from task["file_upload"] or task["data"]["image"]
        fname = task.get("file_upload") or (task.get("data") or {}).get("image", "")
        fname = os.path.basename(fname.replace("\\", "/"))
        img_path = os.path.join(images_dir, fname)
        if not os.path.exists(img_path):
            continue

        annotations = task.get("annotations", [])
        if not annotations:
            continue

        # Use the first completed annotation
        ann = next((a for a in annotations if not a.get("was_cancelled")), None)
        if ann is None:
            continue

        kps: dict[str, tuple[float, float]] = {}
        for result in ann.get("result", []):
            if result.get("type") != "keypointlabels":
                continue
            val    = result["value"]
            label  = val["keypointlabels"][0]
            x_norm = val["x"] / 100.0
            y_norm = val["y"] / 100.0
            kps[label] = (x_norm, y_norm)

        lx, ly = kps.get("LeftWrist",  (0.0, 0.0))
        rx, ry = kps.get("RightWrist", (0.0, 0.0))
        lconf  = 1.0 if "LeftWrist"  in kps else 0.0
        rconf  = 1.0 if "RightWrist" in kps else 0.0

        samples.append(Sample(img_path, lx, ly, lconf, rx, ry, rconf))

    return samples


class WristDataset(Dataset):
    def __init__(self, samples: list[Sample], cfg: Config, augment: bool):
        self.samples = samples
        self.cfg     = cfg
        self.augment = augment

    def __len__(self):
        return len(self.samples)

    def __getitem__(self, idx: int):
        s = self.samples[idx]
        img = Image.open(s.image_path).convert("L")  # grayscale

        lx, ly, lconf = s.lx, s.ly, s.lconf
        rx, ry, rconf = s.rx, s.ry, s.rconf

        if self.augment:
            img, lx, ly, lconf, rx, ry, rconf = self._augment(
                img, lx, ly, lconf, rx, ry, rconf)

        img = img.resize((self.cfg.input_size, self.cfg.input_size), Image.BILINEAR)
        arr = np.array(img, dtype=np.float32) / 255.0          # [0, 1]
        tensor = torch.from_numpy(arr).unsqueeze(0)             # [1, H, W]

        target = torch.tensor([lx, ly, lconf, rx, ry, rconf], dtype=torch.float32)
        return tensor, target

    def _augment(self, img, lx, ly, lconf, rx, ry, rconf):
        cfg = self.cfg

        # Horizontal flip — swap left and right labels
        if random.random() < 0.5:
            img = img.transpose(Image.FLIP_LEFT_RIGHT)
            lx, rx = 1.0 - rx, 1.0 - lx
            ly, ry = ry, ly
            lconf, rconf = rconf, lconf

        # Brightness jitter
        if cfg.brightness_jitter > 0:
            factor = 1.0 + random.uniform(-cfg.brightness_jitter, cfg.brightness_jitter)
            img = ImageEnhance.Brightness(img).enhance(factor)

        # Contrast jitter
        if cfg.contrast_jitter > 0:
            factor = 1.0 + random.uniform(-cfg.contrast_jitter, cfg.contrast_jitter)
            img = ImageEnhance.Contrast(img).enhance(factor)

        # Random crop / zoom (symmetric, ±crop_zoom_jitter of image edge)
        if cfg.crop_zoom_jitter > 0:
            w, h   = img.size
            margin = int(cfg.crop_zoom_jitter * w)
            if margin > 0:
                x0 = random.randint(0, margin)
                y0 = random.randint(0, margin)
                x1 = w - random.randint(0, margin)
                y1 = h - random.randint(0, margin)
                # Transform keypoint coords from original to cropped space
                crop_w = x1 - x0
                crop_h = y1 - y0
                def remap(u, v, conf):
                    if conf < 0.5:
                        return u, v, conf
                    new_u = (u * w - x0) / crop_w
                    new_v = (v * h - y0) / crop_h
                    # If point falls outside crop, zero confidence
                    if new_u < 0 or new_u > 1 or new_v < 0 or new_v > 1:
                        return 0.0, 0.0, 0.0
                    return new_u, new_v, conf
                lx, ly, lconf = remap(lx, ly, lconf)
                rx, ry, rconf = remap(rx, ry, rconf)
                img = img.crop((x0, y0, x1, y1))

        return img, lx, ly, lconf, rx, ry, rconf


# ─── Model ────────────────────────────────────────────────────────────────────

def build_model() -> nn.Module:
    """
    MobileNetV2 alpha=0.35 pretrained on ImageNet, modified for 1-channel input.
    The first conv layer weights are averaged across the 3 RGB channels so
    pretrained low-level feature detectors are preserved.
    """
    base = mobilenet_v2(weights=MobileNet_V2_Weights.IMAGENET1K_V1)

    # Adjust alpha via width_mult — torchvision does not expose alpha directly,
    # so we rebuild with width_mult=0.35.
    from torchvision.models.mobilenetv2 import MobileNetV2
    model_035 = MobileNetV2(width_mult=0.35)

    # Copy pretrained weights where shapes match (first-layer and head will differ)
    pretrained_sd = base.state_dict()
    target_sd     = model_035.state_dict()
    matched = {}
    for k, v in target_sd.items():
        if k in pretrained_sd and pretrained_sd[k].shape == v.shape:
            matched[k] = pretrained_sd[k]
        else:
            matched[k] = v   # keep random init for mismatched layers
    model_035.load_state_dict(matched)

    # Modify first conv: 3-channel → 1-channel by averaging across channel dim.
    first_conv = model_035.features[0][0]  # Conv2d(3, 32, ...)
    orig_weight = first_conv.weight.data    # [out, 3, kH, kW]
    new_conv = nn.Conv2d(1, first_conv.out_channels,
                         kernel_size=first_conv.kernel_size,
                         stride=first_conv.stride,
                         padding=first_conv.padding,
                         bias=False)
    new_conv.weight.data = orig_weight.mean(dim=1, keepdim=True)
    model_035.features[0][0] = new_conv

    # Replace classifier head
    in_features = model_035.classifier[1].in_features
    model_035.classifier = nn.Sequential(
        nn.AdaptiveAvgPool2d((1, 1)),
        nn.Flatten(),
        nn.Linear(in_features, 64),
        nn.ReLU(inplace=True),
        nn.Linear(64, 6),
        nn.Sigmoid(),   # output in [0, 1]
    )

    return model_035


# ─── Loss ─────────────────────────────────────────────────────────────────────

class WristLoss(nn.Module):
    def __init__(self, conf_weight: float = 0.3):
        super().__init__()
        self.conf_weight = conf_weight
        self.mse  = nn.MSELoss(reduction="mean")
        self.bce  = nn.BCELoss(reduction="mean")

    def forward(self, pred: torch.Tensor, target: torch.Tensor):
        # pred, target: [B, 6]  [lx, ly, lconf, rx, ry, rconf]
        # Only compute position MSE for visible wrists (conf > 0.5 in target)
        lconf_gt = target[:, 2:3]
        rconf_gt = target[:, 5:6]

        lmask = (lconf_gt > 0.5).float()
        rmask = (rconf_gt > 0.5).float()

        pos_loss_l = self.mse(pred[:, 0:2] * lmask, target[:, 0:2] * lmask)
        pos_loss_r = self.mse(pred[:, 3:5] * rmask, target[:, 3:5] * rmask)
        pos_loss   = (pos_loss_l + pos_loss_r) * 0.5

        conf_loss_l = self.bce(pred[:, 2], target[:, 2])
        conf_loss_r = self.bce(pred[:, 5], target[:, 5])
        conf_loss   = (conf_loss_l + conf_loss_r) * 0.5

        return pos_loss + self.conf_weight * conf_loss, pos_loss, conf_loss


# ─── Metrics ──────────────────────────────────────────────────────────────────

def pixel_error(pred: torch.Tensor, target: torch.Tensor, input_size: int) -> float:
    """Mean pixel error on visible wrist positions."""
    lmask = target[:, 2] > 0.5
    rmask = target[:, 5] > 0.5
    errors = []
    for mask, pi, ti in [
        (lmask, pred[:, 0:2], target[:, 0:2]),
        (rmask, pred[:, 3:5], target[:, 3:5]),
    ]:
        if mask.sum() > 0:
            diff = (pi[mask] - ti[mask]).pow(2).sum(dim=1).sqrt()
            errors.append((diff * input_size).mean().item())
    return float(np.mean(errors)) if errors else 0.0


def conf_auc(pred: torch.Tensor, target: torch.Tensor) -> float:
    """Approximate AUC on confidence predictions."""
    try:
        from sklearn.metrics import roc_auc_score
        p = torch.cat([pred[:, 2], pred[:, 5]]).detach().cpu().numpy()
        t = torch.cat([target[:, 2], target[:, 5]]).detach().cpu().numpy()
        t_bin = (t > 0.5).astype(int)
        if t_bin.sum() == 0 or t_bin.sum() == len(t_bin):
            return float("nan")
        return float(roc_auc_score(t_bin, p))
    except ImportError:
        return float("nan")


# ─── Training loop ────────────────────────────────────────────────────────────

def train(cfg: Config):
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"Device: {device}")

    # Load data
    print(f"Loading annotations from {cfg.annotations_json}...")
    samples = load_annotations(cfg.annotations_json, cfg.images_dir)
    if not samples:
        raise RuntimeError("No samples loaded — check annotations path and images directory.")
    print(f"  {len(samples)} labelled frames found.")

    random.shuffle(samples)
    n_val   = max(1, int(len(samples) * cfg.val_split))
    val_s   = samples[:n_val]
    train_s = samples[n_val:]
    print(f"  Train: {len(train_s)}  Val: {len(val_s)}")

    train_ds = WristDataset(train_s, cfg, augment=True)
    val_ds   = WristDataset(val_s,   cfg, augment=False)
    train_dl = DataLoader(train_ds, batch_size=cfg.batch_size, shuffle=True,
                          num_workers=cfg.num_workers, pin_memory=True)
    val_dl   = DataLoader(val_ds,   batch_size=cfg.batch_size, shuffle=False,
                          num_workers=0,               pin_memory=True)

    model = build_model().to(device)
    criterion = WristLoss(conf_weight=cfg.confidence_loss_weight)

    # Optimiser: head params only initially
    backbone_params = list(model.features.parameters())
    head_params     = list(model.classifier.parameters())

    def set_backbone_grad(requires: bool):
        for p in backbone_params:
            p.requires_grad = requires

    set_backbone_grad(False)
    optimizer = optim.Adam(
        [{"params": head_params, "lr": cfg.lr_head}],
        weight_decay=cfg.weight_decay
    )
    scheduler = optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=cfg.epochs)

    os.makedirs(cfg.checkpoint_dir, exist_ok=True)
    best_val_err = float("inf")
    best_ckpt    = os.path.join(cfg.checkpoint_dir, "best.pt")

    for epoch in range(1, cfg.epochs + 1):
        # Unfreeze backbone after frozen_epochs
        if epoch == cfg.frozen_epochs + 1:
            print(f"\nEpoch {epoch}: unfreezing backbone, adding to optimizer at lr={cfg.lr_backbone}")
            set_backbone_grad(True)
            optimizer.add_param_group({"params": backbone_params, "lr": cfg.lr_backbone})

        # ── Train ──
        model.train()
        t_loss = t_pos = t_conf = 0.0
        for imgs, targets in train_dl:
            imgs, targets = imgs.to(device), targets.to(device)
            optimizer.zero_grad()
            preds = model(imgs)
            loss, pos_l, conf_l = criterion(preds, targets)
            loss.backward()
            optimizer.step()
            t_loss += loss.item()
            t_pos  += pos_l.item()
            t_conf += conf_l.item()

        scheduler.step()
        n = len(train_dl)
        t_loss /= n; t_pos /= n; t_conf /= n

        # ── Val ──
        model.eval()
        v_loss = v_pos = v_conf = 0.0
        all_preds   = []
        all_targets = []
        with torch.no_grad():
            for imgs, targets in val_dl:
                imgs, targets = imgs.to(device), targets.to(device)
                preds = model(imgs)
                loss, pos_l, conf_l = criterion(preds, targets)
                v_loss += loss.item()
                v_pos  += pos_l.item()
                v_conf += conf_l.item()
                all_preds.append(preds.cpu())
                all_targets.append(targets.cpu())

        nv = len(val_dl)
        v_loss /= nv; v_pos /= nv; v_conf /= nv
        ap = torch.cat(all_preds)
        at = torch.cat(all_targets)
        px_err = pixel_error(ap, at, cfg.input_size)
        auc    = conf_auc(ap, at)

        print(f"Epoch {epoch:3d}/{cfg.epochs}  "
              f"train loss={t_loss:.4f} (pos={t_pos:.4f} conf={t_conf:.4f})  "
              f"val loss={v_loss:.4f}  px_err={px_err:.1f}px  conf_auc={auc:.3f}")

        if px_err < best_val_err:
            best_val_err = px_err
            torch.save({"epoch": epoch, "model_state": model.state_dict(),
                        "val_px_err": px_err, "config": cfg}, best_ckpt)
            print(f"  ✓ best checkpoint saved (px_err={px_err:.1f}px)")

    print(f"\nTraining complete. Best val pixel error: {best_val_err:.1f}px")
    print(f"Exporting best model to ONNX...")
    export_onnx(best_ckpt, cfg, device)


# ─── ONNX export ──────────────────────────────────────────────────────────────

def export_onnx(checkpoint_path: str, cfg: Config, device: torch.device):
    import onnx
    import onnxruntime as ort

    ckpt  = torch.load(checkpoint_path, map_location=device)
    model = build_model().to(device)
    model.load_state_dict(ckpt["model_state"])
    model.eval()

    dummy = torch.zeros(1, 1, cfg.input_size, cfg.input_size, device=device)
    out_path = os.path.join(cfg.checkpoint_dir, cfg.onnx_output)

    torch.onnx.export(
        model,
        dummy,
        out_path,
        opset_version=cfg.onnx_opset,
        input_names=["input"],
        output_names=["output"],
        dynamic_axes=None,   # fixed [1, 1, 192, 192]
        do_constant_folding=True,
    )

    # Validate with onnx checker
    onnx_model = onnx.load(out_path)
    onnx.checker.check_model(onnx_model)
    print(f"  ONNX model valid: {out_path}")

    # Verify with onnxruntime + timing
    sess = ort.InferenceSession(out_path, providers=["CPUExecutionProvider"])
    inp  = dummy.cpu().numpy()

    # Warmup
    for _ in range(5):
        sess.run(None, {"input": inp})

    # Timed runs
    times = []
    for _ in range(50):
        t0 = time.perf_counter()
        out = sess.run(None, {"input": inp})
        times.append((time.perf_counter() - t0) * 1000.0)

    print(f"  OnnxRuntime CPU inference: "
          f"mean={np.mean(times):.2f}ms  p99={np.percentile(times, 99):.2f}ms")
    print(f"  Output shape: {out[0].shape}  values: {out[0]}")
    print(f"  Exported to: {out_path}")


# ─── Entry point ──────────────────────────────────────────────────────────────

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Train PSVR2 wrist detection model")
    parser.add_argument("--annotations", default="annotations.json")
    parser.add_argument("--images",      default="recordings")
    parser.add_argument("--checkpoints", default="checkpoints")
    parser.add_argument("--epochs",      type=int,   default=None)
    parser.add_argument("--batch",       type=int,   default=None)
    args = parser.parse_args()

    cfg = Config(
        annotations_json = args.annotations,
        images_dir       = args.images,
        checkpoint_dir   = args.checkpoints,
    )
    if args.epochs is not None: cfg.epochs     = args.epochs
    if args.batch  is not None: cfg.batch_size = args.batch

    train(cfg)
