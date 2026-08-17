<!-- SPDX-License-Identifier: MPL-2.0 -->

# Quality utilities

- `dependency_license_gate.sh --source-only` checks the MPL license, exact
  dependency declarations, packaging SPDX markers, forbidden FFmpeg options,
  and release-source lock structure. For a distribution candidate, pass
  `--official --audit /path/to/video_editor_dependency_audit`; unresolved source
  locks or a GPL/nonfree FFmpeg runtime then fail the command.
- `format_changed_cpp.sh --check` asks `clang-format` to validate changed and
  untracked C/C++ files. Use `--apply` to update them, and `--base REF` when a CI
  checkout should compare against a particular merge base.
- `verify_corpus.py` verifies every present fixture's safe path, byte count, and
  SHA-256. The default command checks the committed scaffold.
  `generate_corpus.py` materializes the 200+ file synthetic corpus under
  `tests/fixtures/corpus/generated/`. Use `--generated` to check that tree, and
  `--release` only after generation (complete status, 200+ files, all required
  categories).
- `linux_capability_matrix.py` records OS/kernel, Vulkan ICD presence, ffmpeg
  encoder/decoder highlights, and a skip-friendly libplacebo/Vulkan probe. Write
  JSON and text artifacts with `--artifacts DIR`. Missing GPUs are reported, not
  treated as hard failures.
- `validate_flatpak.py` checks the Linux-first Flatpak skeleton: application ID,
  no network permission, desktop/metainfo/svg, and `release-sources.json`.
  Unpinned sources are warnings; `--store` promotes them to failures. Optional
  `flatpak-builder-lint` runs when installed.

These scripts are intentionally local and deterministic. They do not download
models, media, codec binaries, or license data.

