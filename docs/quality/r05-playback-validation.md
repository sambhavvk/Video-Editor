# R05 — Playback and recovery validation

Recorded on branch `beta-1.0-fix` (phase 4 R05), Linux offscreen CI host.

## Automated runs (2026-09-12)

| Suite | Result | Notes |
|-------|--------|-------|
| `video_editor_app_tests` (49 tests) | Pass | Includes preview quality, background job pause, restore points manifest |
| `video_editor_export_service_tests` (19 tests) | Pass | Full-quality export uses originals, limited Rec.709 range |
| `video_editor_render_engine_tests` (60 tests) | 59 pass, 1 fail | `CpuRenderer.CrossDissolveUsesHandleExtrapolationAndHalfOpenRange` pre-existing |
| `app.fault_injection` (ctest) | Not run | Missing `libprotobuf.so.36` in test runner environment |

## Hardware / sync

Physical decode-lab sync/drift/xruns were not measured on this pass (offscreen only). A/V sync
behavior remains covered by existing `EditorControllerTest` playback and audio device poll tests.

## Follow-up

- Re-run fault injection when protobuf runtime is available on the test host.
- Investigate CrossDissolve golden test failure separately from phase 4 scope.
