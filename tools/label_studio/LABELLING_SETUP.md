# Label Studio Setup — PSVR2 Wrist Keypoint Labelling

## Prerequisites

```
pip install label-studio
```

## Start the server (local, no cloud account needed)

```
label-studio start --port 8080
```

Open http://localhost:8080 and create a free local account.

## Create a project

1. Click **Create Project**.
2. Name it `psvr2-wrist-keypoints`.
3. Go to the **Labelling Setup** tab → **Custom template** → paste the contents of `label_studio_config.xml`.
4. Click **Save**.

## Import frames

Label Studio can serve local files when started with the filesystem data directory flag:

```
label-studio start --port 8080 --data-dir C:\path\to\your\recordings
```

Then in the project, **Import** → **Upload Files** — drag in your `_L.png` and `_R.png` files.  
Alternatively use the **Local Storage** source in the project's Cloud Storage settings and point it at your recordings directory.

## Labelling workflow

- **l** key → select LeftWrist, click the left wrist joint centre
- **r** key → select RightWrist, click the right wrist joint centre
- If a wrist is not visible, leave that label unplaced (confidence = 0 implied)
- Use the **zoom** controls to place keypoints accurately — aim for the wrist crease centre
- Frames flagged `motion_flagged: true` in JSON metadata should be skipped or treated carefully

## Export

When done labelling:

1. Project → **Export** → select **JSON** format.
2. Save as `annotations.json` in the `tools/training/` directory.
3. Run `label_assist.py` first to generate pre-annotations and reduce clicking effort — see that script's header.

## Coordinate system

Label Studio exports keypoint `x` and `y` as percentages of image width/height (0–100).  
The training pipeline (`train.py`) converts these to normalised UV [0, 1] automatically.
