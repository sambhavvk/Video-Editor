# V03 — Portable project travel kit

## Automated evidence

- `TravelKitTest.WritesProjectAndManifest` — kit folder contains serialized project and asset manifest.

## Manual verification

1. Build a travel kit from a project with relative media paths.
2. Copy the kit to a clean machine and open `project.veproj`.
3. Confirm `asset_manifest.txt` lists bundled and omitted dependencies.

## Limitations (V03)

- Remote URIs are listed but not copied; only local relative files are bundled.
