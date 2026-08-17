#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0

"""Report Linux OS, Vulkan ICD, and ffmpeg codec capabilities without failing on missing GPUs."""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any


HIGHLIGHT_ENCODERS = (
    "libvpx-vp9",
    "libopus",
    "ffv1",
    "prores",
    "prores_ks",
    "vp9_vaapi",
)

COMMON_DECODERS = (
    "h264",
    "hevc",
    "vp8",
    "vp9",
    "av1",
    "mpeg4",
    "mpeg2video",
    "mjpeg",
    "prores",
    "ffv1",
    "png",
    "aac",
    "opus",
    "vorbis",
    "flac",
    "mp3",
    "pcm_s16le",
    "ac3",
    "eac3",
)

ICD_DIRECTORIES = (
    Path("/usr/share/vulkan/icd.d"),
    Path("/etc/vulkan/icd.d"),
    Path.home() / ".local/share/vulkan/icd.d",
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts",
        type=Path,
        default=None,
        help="optional directory for linux-capability-matrix.json and .txt",
    )
    return parser.parse_args()


def run_command(argv: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(argv, check=False, capture_output=True, text=True)


def read_os_release() -> dict[str, str]:
    path = Path("/etc/os-release")
    values: dict[str, str] = {}
    if not path.is_file():
        return values
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" not in line or line.startswith("#"):
            continue
        key, value = line.split("=", 1)
        values[key] = value.strip().strip('"')
    return values


def os_report() -> dict[str, Any]:
    release = read_os_release()
    uname = run_command(["uname", "-a"])
    return {
        "system": platform.system(),
        "release": platform.release(),
        "version": platform.version(),
        "machine": platform.machine(),
        "prettyName": release.get("PRETTY_NAME") or f"{platform.system()} {platform.release()}",
        "uname": uname.stdout.strip() if uname.returncode == 0 else "",
    }


def list_icd_files() -> list[str]:
    found: list[str] = []
    extra = os.environ.get("VK_ICD_FILENAMES", "")
    candidates = [Path(part) for part in extra.split(":") if part]
    for directory in ICD_DIRECTORIES:
        if directory.is_dir():
            candidates.extend(sorted(directory.glob("*.json")))
    for path in candidates:
        if path.is_file():
            found.append(str(path))
    return found


def vulkan_report() -> dict[str, Any]:
    icds = list_icd_files()
    vulkaninfo = shutil.which("vulkaninfo")
    summary = ""
    error = ""
    available = False
    if vulkaninfo:
        completed = run_command([vulkaninfo, "--summary"])
        if completed.returncode == 0 and completed.stdout.strip():
            summary = completed.stdout.strip()
            available = True
        else:
            error = (completed.stderr or completed.stdout or "vulkaninfo --summary failed").strip()
    elif icds:
        error = "vulkaninfo is not installed; reporting ICD JSON files only"
    else:
        error = "no vulkaninfo and no ICD JSON files found"

    if available:
        status = "available"
    elif icds:
        status = "skip"
    else:
        status = "unavailable"

    return {
        "status": status,
        "icdFiles": icds,
        "icdDirectories": [str(path) for path in ICD_DIRECTORIES],
        "vulkaninfo": {
            "available": vulkaninfo is not None,
            "summary": summary,
            "error": error,
        },
    }


def parse_codec_list(stdout: str) -> set[str]:
    names: set[str] = set()
    for line in stdout.splitlines():
        parts = line.split()
        if len(parts) < 2 or parts[0][0] not in {"V", "A", "S"}:
            continue
        if "." not in parts[0]:
            continue
        names.add(parts[1])
    return names


def ffmpeg_report() -> dict[str, Any]:
    ffmpeg = os.environ.get("FFMPEG") or shutil.which("ffmpeg")
    if not ffmpeg:
        return {
            "available": False,
            "version": "",
            "encoders": {name: False for name in HIGHLIGHT_ENCODERS},
            "decoders": {name: False for name in COMMON_DECODERS},
            "error": "ffmpeg is not on PATH; set FFMPEG or install ffmpeg",
        }

    version = run_command([ffmpeg, "-hide_banner", "-version"])
    encoders = run_command([ffmpeg, "-hide_banner", "-encoders"])
    decoders = run_command([ffmpeg, "-hide_banner", "-decoders"])
    encoder_names = parse_codec_list(encoders.stdout) if encoders.returncode == 0 else set()
    decoder_names = parse_codec_list(decoders.stdout) if decoders.returncode == 0 else set()
    version_line = version.stdout.splitlines()[0].strip() if version.stdout else ""
    return {
        "available": True,
        "version": version_line,
        "encoders": {name: name in encoder_names for name in HIGHLIGHT_ENCODERS},
        "decoders": {name: name in decoder_names for name in COMMON_DECODERS},
        "error": "",
    }


def libplacebo_report(vulkan: dict[str, Any]) -> dict[str, Any]:
    details: list[str] = []
    version = ""
    pkg = shutil.which("pkg-config")
    if pkg:
        completed = run_command([pkg, "--modversion", "libplacebo"])
        if completed.returncode == 0 and completed.stdout.strip():
            version = completed.stdout.strip()
            details.append(f"pkg-config libplacebo {version}")
        else:
            details.append("pkg-config did not report libplacebo")
    else:
        details.append("pkg-config is not available")

    ldconfig = shutil.which("ldconfig")
    if ldconfig:
        completed = run_command([ldconfig, "-p"])
        if completed.returncode == 0 and "libplacebo" in completed.stdout:
            details.append("ldconfig lists a libplacebo shared library")
        else:
            details.append("ldconfig does not list libplacebo")

    if version and vulkan["status"] == "available":
        status = "available"
    elif version or vulkan["status"] in {"available", "skip"}:
        status = "skip"
        details.append("libplacebo/Vulkan looks only partially usable; treat as skip-friendly")
    else:
        status = "unavailable"
        details.append("no libplacebo module and no usable Vulkan ICD")

    return {
        "status": status,
        "version": version or None,
        "detail": "; ".join(details),
    }


def render_text(report: dict[str, Any]) -> str:
    os_info = report["os"]
    vulkan = report["vulkan"]
    ffmpeg = report["ffmpeg"]
    placebo = report["libplacebo"]
    lines = [
        "Linux GPU/codec capability matrix",
        f"OS: {os_info['prettyName']} ({os_info['machine']})",
        f"Kernel: {os_info['release']}",
        f"Vulkan: {vulkan['status']}",
    ]
    if vulkan["icdFiles"]:
        lines.append("Vulkan ICDs:")
        lines.extend(f"  - {path}" for path in vulkan["icdFiles"])
    else:
        lines.append("Vulkan ICDs: none found under /usr/share/vulkan/icd.d or /etc/vulkan/icd.d")
    if vulkan["vulkaninfo"]["summary"]:
        lines.append("vulkaninfo --summary:")
        lines.extend(f"  {line}" for line in vulkan["vulkaninfo"]["summary"].splitlines())
    elif vulkan["vulkaninfo"]["error"]:
        lines.append(f"vulkaninfo: {vulkan['vulkaninfo']['error']}")

    if ffmpeg["available"]:
        lines.append(f"ffmpeg: {ffmpeg['version']}")
        lines.append("Highlighted encoders:")
        for name, present in ffmpeg["encoders"].items():
            lines.append(f"  - {name}: {'yes' if present else 'no'}")
        lines.append("Common decoders:")
        for name, present in ffmpeg["decoders"].items():
            lines.append(f"  - {name}: {'yes' if present else 'no'}")
    else:
        lines.append(f"ffmpeg: unavailable ({ffmpeg['error']})")

    lines.append(f"libplacebo/Vulkan: {placebo['status']}")
    lines.append(f"  {placebo['detail']}")
    lines.append("Missing GPU hardware is recorded as skip/unavailable and is not a hard failure.")
    return "\n".join(lines) + "\n"


def main() -> int:
    arguments = parse_arguments()
    vulkan = vulkan_report()
    report = {
        "schemaVersion": 1,
        "os": os_report(),
        "vulkan": vulkan,
        "ffmpeg": ffmpeg_report(),
        "libplacebo": libplacebo_report(vulkan),
    }
    text = render_text(report)
    payload = json.dumps(report, indent=2) + "\n"
    sys.stdout.write(text)
    sys.stdout.write("\n=== JSON ===\n")
    sys.stdout.write(payload)

    if arguments.artifacts is not None:
        arguments.artifacts.mkdir(parents=True, exist_ok=True)
        (arguments.artifacts / "linux-capability-matrix.txt").write_text(text, encoding="utf-8")
        (arguments.artifacts / "linux-capability-matrix.json").write_text(payload, encoding="utf-8")
        print(f"Wrote artifacts under {arguments.artifacts}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
