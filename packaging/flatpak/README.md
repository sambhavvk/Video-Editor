<!-- SPDX-License-Identifier: MPL-2.0 -->

# Flatpak packaging

Flatpak is the **Linux-first public beta packaging target**. This directory is
a development skeleton, not a store-ready submission. Immutable checksummed
release archives, a reviewed LGPL dependency lock, and a stable Flathub /
reverse-DNS identity remain blockers. Do not invent an owned homepage URL; the
AppStream file omits one until the project actually has a public page.

Validate the skeleton (no network permission, metadata files, source lock):

```sh
python3 tools/quality/validate_flatpak.py
```

That command is allowed to warn about `releaseBlocking` and unpinned sources.
`--store` promotes those warnings to failures. If `flatpak-builder-lint` is
installed it is run and its known identity/homepage findings stay warnings for
the Linux-first check:

```sh
flatpak-builder-lint manifest packaging/flatpak/org.videoeditor.VideoEditor.yml
```

`org.videoeditor.VideoEditor.yml` is a development packaging skeleton for the
current KDE 6.11 runtime branch. It intentionally has no network permission and
uses Wayland with X11 fallback, PulseAudio, DRI, creator folders, and common
removable-media mount points. Normal file selection should continue to use the
desktop portal.

The application ID is provisional until the project has an owned reverse-DNS
identity. Changing it after public distribution breaks desktop identity, so that
decision is a release blocker. The AppStream file likewise omits a homepage
instead of publishing a made-up URL; strict validation reports
`url-homepage-missing` until the project owns a stable public page.

## Local smoke build

The manifest uses the local repository via a `dir` source. From the repository
root, a developer with the runtimes and `flatpak-builder` installed can try:

```sh
flatpak-builder --user --install-deps-from=flathub --force-clean \
  build/flatpak packaging/flatpak/org.videoeditor.VideoEditor.yml
```

This is not yet expected to complete in a clean SDK. The application currently
requires exact dependency ABIs, including the approved LGPL FFmpeg 9.0.1 build,
that are not bundled by this skeleton. A configure failure is therefore an
honest indication that the release dependency lock is incomplete.

Before any distributable build:

1. Replace the local `dir` source with an immutable release archive.
2. Fill every URL and SHA-256 in `release-sources.json`; do not use example or
   placeholder URLs.
3. Set `releaseBlocking` to `false` only after every entry is `pinned` and the
   dependency/license gate passes with `--official`.
4. Replace the textual SVG development icon with approved artwork, retaining a
   scalable SVG source and generated raster sizes if the store requires them.
5. Run `flatpak-builder-lint`, AppStream validation, the official dependency
   audit, and the codec/GPU test matrix in the actual release environment.
