# SoaSim Blender IR Importer: Install and Use

This folder contains a baseline Blender add-on script:

- `soasim_blender_ir_importer.py`

## Install in Blender

1. Open Blender.
2. Go to **Edit → Preferences → Add-ons**.
3. Click **Install...**.
4. Select `SoaSimMLD/blender/soasim_blender_ir_importer.py`.
5. Enable the add-on named **SoaSim Blender IR Importer**.

Alternative manual install:

- Copy `soasim_blender_ir_importer.py` to your Blender add-ons folder (for your Blender version), then enable it from **Preferences → Add-ons**.

## Access after install

Once enabled, open:

- **File → Import → SoaSim Blender IR (.json)**

That opens the importer operator (`import_scene.soasim_blender_ir`) with options:

- **Collection** (target collection name, default `SoaSim_Imported`)
- **Clear Target Collection** (clear previous imported objects before loading)
- **Emit Parity Debug Log** (writes per-entry/per-node local + world transform matrices into a Blender Text datablock named `<Collection>_ParityDebug`)

Then select your exported `blender_ir_scene.json` and import.

## What this baseline importer handles

- Mesh vertices (positions)
- Triangle indices from triangle-set corners
- Basic materials and texture hookups by `textureName`
- Decoded RGBA8 texture payloads (`pixelDataBase64`)
- Instance placement from `indexEntries` transforms

It is intentionally baseline and aligned to current JSON export.
