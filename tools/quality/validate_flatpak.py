#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0

"""Validate the Linux-first Flatpak packaging skeleton without claiming store readiness."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path


SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
PLACEHOLDER_MARKERS = (
    "example.com",
    "example.org",
    "example.net",
    "placeholder",
    "your-domain",
    "yourdomain",
    "localhost",
    "invalid.",
    "todo",
)
NETWORK_FINISH_ARG = re.compile(r"^\s*-\s*--share=network\b")


class Finding:
    def __init__(self, severity: str, message: str) -> None:
        self.severity = severity
        self.message = message


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--store",
        action="store_true",
        help="treat release blockers and lint warnings as failures (store-ready gate)",
    )
    return parser.parse_args()


def repository_root() -> Path:
    return Path(__file__).resolve().parents[2]


def is_placeholder_url(value: str) -> bool:
    lowered = value.lower()
    return any(marker in lowered for marker in PLACEHOLDER_MARKERS)


def validate_yaml_and_metadata(flatpak_dir: Path, findings: list[Finding]) -> None:
    manifest = flatpak_dir / "org.videoeditor.VideoEditor.yml"
    desktop = flatpak_dir / "org.videoeditor.VideoEditor.desktop"
    metainfo = flatpak_dir / "org.videoeditor.VideoEditor.metainfo.xml"
    icon = flatpak_dir / "org.videoeditor.VideoEditor.svg"
    if not manifest.is_file():
        findings.append(Finding("FAIL", f"missing {manifest}"))
        return
    text = manifest.read_text(encoding="utf-8")
    if "app-id: org.videoeditor.VideoEditor" not in text:
        findings.append(Finding("FAIL", "manifest application ID must remain org.videoeditor.VideoEditor"))
    if NETWORK_FINISH_ARG.search(text):
        findings.append(Finding("FAIL", "Flatpak manifest must not grant --share=network"))
    else:
        findings.append(Finding("OK", "no network permission in finish-args"))
    for label, path in (
        ("desktop", desktop),
        ("metainfo", metainfo),
        ("svg icon", icon),
    ):
        if path.is_file():
            findings.append(Finding("OK", f"{label} present: {path.name}"))
        else:
            findings.append(Finding("FAIL", f"missing {label}: {path}"))


def validate_release_sources(lock_path: Path, schema_path: Path, findings: list[Finding]) -> None:
    if not lock_path.is_file():
        findings.append(Finding("FAIL", f"missing {lock_path}"))
        return
    if not schema_path.is_file():
        findings.append(Finding("FAIL", f"missing {schema_path}"))
        return
    try:
        data = json.loads(lock_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        findings.append(Finding("FAIL", f"cannot read {lock_path}: {exc}"))
        return

    if not isinstance(data, dict):
        findings.append(Finding("FAIL", "release-sources.json root must be an object"))
        return
    if data.get("license") != "MPL-2.0" or data.get("schemaVersion") != 1:
        findings.append(Finding("FAIL", "release-sources.json license/schemaVersion are invalid"))
    if not isinstance(data.get("releaseBlocking"), bool):
        findings.append(Finding("FAIL", "releaseBlocking must be a boolean"))
    if not isinstance(data.get("reason"), str) or not data["reason"]:
        findings.append(Finding("FAIL", "reason must be a non-empty string"))

    sources = data.get("sources")
    if not isinstance(sources, list) or not sources:
        findings.append(Finding("FAIL", "sources must be a non-empty array"))
        return

    findings.append(Finding("OK", "release-sources.json matches the required lock shape"))
    unpinned: list[str] = []
    placeholders: list[str] = []
    for index, source in enumerate(sources):
        label = f"sources[{index}]"
        if not isinstance(source, dict):
            findings.append(Finding("FAIL", f"{label} must be an object"))
            continue
        name = source.get("name")
        status = source.get("status")
        url = source.get("archiveUrl")
        digest = source.get("sha256")
        resolution = source.get("requiredResolution")
        if not isinstance(name, str) or not name:
            findings.append(Finding("FAIL", f"{label}.name is required"))
            name = label
        if status not in {"release-blocking", "pinned"}:
            findings.append(Finding("FAIL", f"{label}.status must be release-blocking or pinned"))
        if not isinstance(resolution, str) or not resolution:
            findings.append(Finding("FAIL", f"{label}.requiredResolution is required"))
        if url is not None and (not isinstance(url, str) or not url.startswith("https://")):
            findings.append(Finding("FAIL", f"{label}.archiveUrl must be null or an https URL"))
        if digest is not None and (not isinstance(digest, str) or not SHA256_PATTERN.fullmatch(digest)):
            findings.append(Finding("FAIL", f"{label}.sha256 must be null or a lowercase SHA-256"))
        if isinstance(url, str) and is_placeholder_url(url):
            placeholders.append(name)
        pinned = (
            status == "pinned"
            and isinstance(url, str)
            and url.startswith("https://")
            and not is_placeholder_url(url)
            and isinstance(digest, str)
            and SHA256_PATTERN.fullmatch(digest) is not None
        )
        if not pinned:
            unpinned.append(name)

    release_blocking = data.get("releaseBlocking") is True
    if placeholders:
        findings.append(
            Finding(
                "FAIL",
                "placeholder release URLs are not allowed: " + ", ".join(placeholders),
            )
        )
    if release_blocking:
        findings.append(
            Finding(
                "WARN",
                "releaseBlocking is true; Flatpak is a Linux-first packaging target, not store-ready",
            )
        )
    if unpinned:
        findings.append(
            Finding(
                "WARN",
                "unpinned release sources remain blockers: " + ", ".join(unpinned),
            )
        )
    if not release_blocking and unpinned:
        findings.append(
            Finding(
                "FAIL",
                "releaseBlocking is false but source locks are still unpinned",
            )
        )


def run_optional_lint(manifest: Path, findings: list[Finding]) -> None:
    lint = shutil.which("flatpak-builder-lint")
    if not lint:
        findings.append(Finding("SKIP", "flatpak-builder-lint is not installed"))
        return
    completed = subprocess.run(
        [lint, "manifest", str(manifest)],
        check=False,
        capture_output=True,
        text=True,
    )
    output = (completed.stdout + completed.stderr).strip()
    if completed.returncode == 0:
        findings.append(Finding("OK", "flatpak-builder-lint passed"))
        return
    detail = output or f"exit {completed.returncode}"
    findings.append(
        Finding(
            "WARN",
            "flatpak-builder-lint reported issues (expected until homepage/identity exist): "
            + detail.replace("\n", " | "),
        )
    )


def main() -> int:
    arguments = parse_arguments()
    root = repository_root()
    flatpak_dir = root / "packaging/flatpak"
    findings: list[Finding] = []
    validate_yaml_and_metadata(flatpak_dir, findings)
    validate_release_sources(
        flatpak_dir / "release-sources.json",
        flatpak_dir / "release-sources.schema.json",
        findings,
    )
    run_optional_lint(flatpak_dir / "org.videoeditor.VideoEditor.yml", findings)

    store_promoted = 0
    if arguments.store:
        for finding in findings:
            if finding.severity == "WARN":
                finding.severity = "FAIL"
                store_promoted += 1

    counts = {"FAIL": 0, "WARN": 0, "OK": 0, "SKIP": 0}
    for finding in findings:
        print(f"{finding.severity}: {finding.message}")
        counts[finding.severity] = counts.get(finding.severity, 0) + 1

    failed = counts["FAIL"]
    if failed:
        print(
            f"Flatpak validation failed with {failed} error(s), "
            f"{counts['WARN']} warning(s).",
            file=sys.stderr,
        )
        return 1

    print(
        "Linux-first Flatpak validation passed with "
        f"{counts['WARN']} warning(s). Store/Flathub identity and checksummed "
        "release archives remain blockers."
    )
    if store_promoted:
        print(f"Promoted {store_promoted} warning(s) under --store.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
