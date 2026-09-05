#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Physical A/V lab runner for the 1-hour zero-xrun and 2-hour 10 ms drift gates.

Accelerated fake-device tests (VE_RUN_LONG_TESTS=1) do not satisfy these gates.
Calibrate the mixer output first, then run against a real miniaudio device.

Examples:
  python3 tools/quality/physical_av_lab.py --seconds 5 --output /tmp/av-lab.json
  python3 tools/quality/physical_av_lab.py --hours 1 --gate xrun
  python3 tools/quality/physical_av_lab.py --hours 2 --gate drift --output evidence.json
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--gtest",
        default="",
        help="Path to video_editor_audio_engine_tests (default: search build/dev)",
    )
    parser.add_argument("--seconds", type=int, default=0, help="Override duration in seconds")
    parser.add_argument("--hours", type=float, default=0.0, help="Duration in hours")
    parser.add_argument(
        "--gate",
        choices=("smoke", "xrun", "drift"),
        default="smoke",
        help="smoke=short evidence, xrun=1h zero-xrun, drift=2h <10 ms",
    )
    parser.add_argument("--output", default="", help="JSON evidence path")
    return parser.parse_args()


def resolve_gtest(explicit: str) -> Path:
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise SystemExit(f"gtest binary not found: {path}")
        return path
    candidates = [
        Path("build/dev/tests/audio_engine/video_editor_audio_engine_tests"),
        Path("build/tests/audio_engine/video_editor_audio_engine_tests"),
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise SystemExit("Could not find video_editor_audio_engine_tests; pass --gtest")


def duration_seconds(args: argparse.Namespace) -> int:
    if args.seconds > 0:
        return args.seconds
    if args.hours > 0:
        return max(1, int(args.hours * 3600))
    if args.gate == "xrun":
        return 3600
    if args.gate == "drift":
        return 7200
    return 5


def main() -> int:
    args = parse_args()
    gtest = resolve_gtest(args.gtest)
    seconds = duration_seconds(args)
    output = args.output or str(Path("physical-av-lab.json").resolve())
    env = os.environ.copy()
    env["VIDEO_EDITOR_PHYSICAL_AV_LAB"] = "1"
    env["VIDEO_EDITOR_PHYSICAL_AV_LAB_SECONDS"] = str(seconds)
    env["VIDEO_EDITOR_PHYSICAL_AV_LAB_OUT"] = output
    command = [
        str(gtest),
        "--gtest_filter=PhysicalAvLab.RecordsXrunAndWallClockDriftEvidence",
    ]
    print(f"running {' '.join(command)} for {seconds}s; evidence -> {output}", file=sys.stderr)
    completed = subprocess.run(command, env=env, check=False)
    if completed.returncode != 0:
        return completed.returncode
    payload = json.loads(Path(output).read_text(encoding="utf-8"))
    xruns = int(payload.get("xrun_count", -1))
    error_ms = float(payload.get("av_error_ms", 1.0e9))
    print(json.dumps(payload, indent=2))
    if args.gate in ("xrun", "drift") and xruns != 0:
        print(f"xrun gate failed: {xruns} xruns", file=sys.stderr)
        return 2
    if args.gate == "drift" and error_ms >= 10.0:
        print(f"drift gate failed: {error_ms:.3f} ms", file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
