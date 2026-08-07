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
  SHA-256. Add `--release` only once the manifest is complete.

These scripts are intentionally local and deterministic. They do not download
models, media, codec binaries, or license data.

