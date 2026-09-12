<!-- SPDX-License-Identifier: MPL-2.0 -->

# B02: FOSS component register

Recorded 12 September 2026. Authoritative version pins live in
`cmake/DependencyVersions.cmake`. This register is the eligibility record for
selected components. It does not add or replace dependencies.

**Open-stack execution path (Linux-first beta):** CPU preview and software
encoders (`libvpx-vp9`, `libopus`, optional `libsvtav1`/`libaom-av1`) on an
LGPL FFmpeg 9.0.1 build (`--disable-gpl --disable-nonfree`), dynamically linked
LGPL Qt 6.11.2 and libplacebo 7.360.1, optional Vulkan 1.2 presentation with
CPU fallback. Proprietary drivers, closed codec SDKs, and paid APIs are not
required.

Distribution gates stay in force: `tools/quality/dependency_license_gate.sh`,
`video_editor_dependency_audit --official`,
`VIDEO_EDITOR_H264_DELIVERY_APPROVED` / `VIDEO_EDITOR_AAC_DELIVERY_APPROVED`
(default off), and `packaging/flatpak/release-sources.json` (`releaseBlocking`).

## Selected components

| Component | Pin | License | Link | Official path | Verdict |
| --- | --- | --- | --- | --- | --- |
| VideoEditor | 0.1.0 | MPL-2.0 | source | same | Eligible |
| Qt 6 (Core, Concurrent, Gui, Widgets, Network, Test) | 6.11.2 exact | LGPL-3.0-only | dynamic | LGPL modules only | Eligible |
| FFmpeg libav* | 9.0.1; avformat/avcodec 63.1.101; avutil 61.1.101; swresample 7.1.101; swscale 10.1.101 | LGPL-2.1-or-later (official) | dynamic | `--disable-gpl --disable-nonfree`; local GPL FFmpeg is **dev/tests only** | Eligible if official config; **blocked** if gpl/nonfree loaded |
| libplacebo | 7.360.1 exact | LGPL-2.1-or-later | dynamic, optional | Vulkan (Linux) / D3D11 (Windows preview); CPU fallback | Eligible |
| Vulkan headers/loader | unpinned API 1.2+ | Apache-2.0 (headers/loader) | system ICD | not bundled; vendor ICD is user-provided | Eligible (open API); ICD not a project dependency |
| miniaudio | 0.11.25 | MIT-0 | source-integrated header; decode disabled | FetchContent or `MINIAUDIO_ROOT` | Eligible |
| SQLite | ≥ 3.45 (WAL) | blessing | dynamic | system/runtime | Eligible |
| Protocol Buffers | 35.1 | BSD-3-Clause | dynamic (official); static in some CI | generated sources stay in the build tree | Eligible |
| Abseil | 20250512.1 | Apache-2.0 | static (Protobuf support) | CI/release lock | Eligible |
| OpenSSL libcrypto | ≥ 3.0 | Apache-2.0 | dynamic | hashing only | Eligible |
| libebur128 | 1.2.6 | MIT | dynamic | loudness | Eligible |
| HarfBuzz | 14.3.1 | MIT | dynamic | caption/title shaping | Eligible |
| FreeType | 26.6.20 | FTL | dynamic | caption/title raster | Eligible |
| Noto Sans Regular | SHA-256 `478c558ea716033cd60c03438f628dfa75694dcf6b5f6d505a2f05fd2b4f3823`, 621572 bytes | OFL-1.1 | bundled TTF | platform fonts never consulted | Eligible |
| whisper.cpp | 1.9.2 @ `306c88f4d1286aec1bf96e544632897886af5501` | MIT | optional dynamic | off unless exact library supplied | Eligible; official binary lock still **deferred** |
| ggml-base.bin | `base`, 147951465 bytes, SHA-1 `465707469ff3a37a2b9b8d8f89f2f99de7299dac` | MIT (OpenAI Whisper weights; ggml conversion) | on-demand download, not in git | user-initiated, checksummed | Eligible FOSS; **not redistributed** until official packaging includes MIT notices. Manual captions remain complete without it. |
| GoogleTest | 1.17 (docs); `find_package(GTest)` | BSD-3-Clause | tests-only | omitted from Flatpak (`BUILD_TESTS=OFF`) | Eligible (not shipped) |
| In-tree OTIO JSON | n/a | MPL-2.0 | first-party | no OpenTimelineIO library | Eligible |
| Flatpak KDE Platform | runtime branch 6.11 | LGPL (runtime) | Flatpak | unpinned archives are **release-blocking** | Deferred until `release-sources.json` is pinned |
| Dev icon SVG | n/a | CC0-1.0 | packaging | replace before store | Eligible |
| Synthetic corpus | lavfi recipes | MPL-2.0 generated | tests-only | gitignored binaries | Eligible |

Redistribution: official packages must keep license texts, MPL notices, LGPL source offers for Qt/FFmpeg/libplacebo, OFL for Noto Sans, and an SPDX SBOM from the `sbom` target.

## Codec implementations (via FFmpeg)

Not separate CMake packages. Official delivery may use only LGPL-compatible encoders.

| Implementation | Role | Official |
| --- | --- | --- |
| libvpx-vp9 | Creator video (default) | Required FOSS path |
| libopus | Creator/podcast audio | Required when the preset has audio |
| libsvtav1 / libaom-av1 | Creator AV1 | Optional FOSS path |
| vp9_vaapi / av1_vaapi / vp9_qsv | Hardware encode | Optional; software retry required |
| ffv1, prores_ks/aw | Reference masters | Allowed |
| libx264, h264_*, libfdk_aac, aac | Delivery | **Blocked** until legal flags and specialist review |
| H.264/HEVC/AV1/VP9 **decoders** | Import/playback | Allowed in LGPL FFmpeg; patent review remains a separate gate |

## Blocked or deferred (not selected)

| Item | Status | Reason |
| --- | --- | --- |
| GPL/nonfree FFmpeg | Blocked for official packages | License gate and runtime audit |
| H.264/AAC delivery | Blocked | `VIDEO_EDITOR_H264_DELIVERY_APPROVED` / `AAC` default OFF; legal/patent gate |
| Unpinned Flatpak archives | Blocked for `--official` / `--store` | `release-sources.json` `releaseBlocking: true` |
| whisper.cpp official binary lock | Deferred | Optional local build only; no reviewed redistributable `.so` |
| ggml-base.bin in the installer | Deferred | Eligible MIT weights, but not bundled until notices and the binary lock exist |
| Windows MSI runtime harvest | Deferred | After Linux-first beta |
| Vendor Vulkan ICD / proprietary GPU driver | Not required | CPU path is the open-stack fallback |
| OpenTimelineIO upstream library, AAF/EDL/XML SDKs | Not selected | First-party OTIO JSON only |
| Closed camera-RAW SDKs, paid cloud AI, proprietary plugins | Not selected | Roadmap X-chunks may propose FOSS alternatives later |

## Proposed (roadmap) — not selected

Discovery chunks may later consider EDL/AAF/XML libraries, tracking/stabilization, extra local models, open RAW, HDR, surround, or a plugin host. None is in the selected set. Each would need its own license and redistribution review before B02-style selection.

## Local ABI vs contract

This machine's CachyOS packages linked **Protobuf 36.1** while the source contract is **35.1**. Local development may use ABI-identical or newer packages; official packages must match `cmake/DependencyVersions.cmake`. The development SBOM currently hardcodes SQLite 3.53.4 and OpenSSL 3.6.3 as examples; the contract is SQLite ≥ 3.45 and OpenSSL ≥ 3.0.

## How to re-check

```sh
tools/quality/dependency_license_gate.sh --source-only
cmake --build build/dev --target sbom
# Official candidate only:
# tools/quality/dependency_license_gate.sh --official --audit build/dev/src/media_codec/video_editor_dependency_audit
```
