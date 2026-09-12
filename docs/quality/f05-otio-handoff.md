# F05 — OTIO handoff report and package

## Automated evidence

- `OtioHandoffTest.BuildsPackageWithReportSections` — handoff folder contains timeline, manifest, and categorized report sections.

## Manual verification

1. Export an OTIO handoff package from a sequence with effects and nested clips.
2. Open `handoff_report.txt` and confirm supported, baked, flattened, and omitted sections list every non-parity element.
3. Open `timeline.otio` in an OpenTimelineIO-capable receiver and verify clip timing matches the source sequence.

## Limitations (F05)

- Media files are referenced, not copied, in the default package; use the travel kit (V03) for bundled media.
