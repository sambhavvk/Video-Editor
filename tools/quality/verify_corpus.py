#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0

"""Validate the representative media corpus paths, sizes, and SHA-256 values."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")


class ValidationError(RuntimeError):
    """A corpus manifest or fixture failed validation."""


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=None,
        help="manifest path (defaults to tests/fixtures/corpus/manifest.json)",
    )
    parser.add_argument(
        "--release",
        action="store_true",
        help="require complete beta corpus coverage",
    )
    return parser.parse_args()


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValidationError(f"cannot read {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise ValidationError("manifest root must be an object")
    return data


def validate_structure(data: dict[str, Any]) -> tuple[list[str], list[dict[str, Any]]]:
    if data.get("schemaVersion") != 1:
        raise ValidationError("schemaVersion must be 1")
    if data.get("license") != "MPL-2.0":
        raise ValidationError("manifest license must be MPL-2.0")
    if data.get("status") not in {"scaffold", "complete"}:
        raise ValidationError("status must be scaffold or complete")
    if not isinstance(data.get("minimumAssetCount"), int) or data["minimumAssetCount"] < 200:
        raise ValidationError("minimumAssetCount must be an integer of at least 200")

    required = data.get("requiredCategories")
    assets = data.get("assets")
    if not isinstance(required, list) or not required or not all(
        isinstance(category, str) and category for category in required
    ):
        raise ValidationError("requiredCategories must be a non-empty string array")
    if len(required) != len(set(required)):
        raise ValidationError("requiredCategories contains duplicates")
    if not isinstance(assets, list):
        raise ValidationError("assets must be an array")
    return required, assets


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fixture:
        for block in iter(lambda: fixture.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def validate_assets(manifest_directory: Path, assets: list[dict[str, Any]]) -> set[str]:
    corpus_root = manifest_directory.resolve()
    seen_ids: set[str] = set()
    seen_paths: set[str] = set()
    covered_categories: set[str] = set()

    for index, asset in enumerate(assets):
        label = f"assets[{index}]"
        if not isinstance(asset, dict):
            raise ValidationError(f"{label} must be an object")

        asset_id = asset.get("id")
        relative_path = asset.get("path")
        expected_bytes = asset.get("bytes")
        expected_digest = asset.get("sha256")
        categories = asset.get("categories")
        provenance = asset.get("provenance")

        if not isinstance(asset_id, str) or not asset_id:
            raise ValidationError(f"{label}.id must be a non-empty string")
        if asset_id in seen_ids:
            raise ValidationError(f"duplicate asset id: {asset_id}")
        seen_ids.add(asset_id)

        if not isinstance(relative_path, str) or not relative_path.startswith("files/"):
            raise ValidationError(f"{label}.path must begin with files/")
        if relative_path in seen_paths:
            raise ValidationError(f"duplicate asset path: {relative_path}")
        seen_paths.add(relative_path)

        resolved_path = (manifest_directory / relative_path).resolve()
        if corpus_root not in resolved_path.parents:
            raise ValidationError(f"path escapes corpus root: {relative_path}")
        if not resolved_path.is_file():
            raise ValidationError(f"fixture is missing: {relative_path}")

        if not isinstance(expected_bytes, int) or expected_bytes < 0:
            raise ValidationError(f"{label}.bytes must be a non-negative integer")
        actual_bytes = resolved_path.stat().st_size
        if actual_bytes != expected_bytes:
            raise ValidationError(
                f"size mismatch for {relative_path}: expected {expected_bytes}, got {actual_bytes}"
            )

        if not isinstance(expected_digest, str) or not SHA256_PATTERN.fullmatch(expected_digest):
            raise ValidationError(f"{label}.sha256 must be a lowercase SHA-256")
        actual_digest = hash_file(resolved_path)
        if actual_digest != expected_digest:
            raise ValidationError(
                f"checksum mismatch for {relative_path}: expected {expected_digest}, "
                f"got {actual_digest}"
            )

        if not isinstance(categories, list) or not categories or not all(
            isinstance(category, str) and category for category in categories
        ):
            raise ValidationError(f"{label}.categories must be a non-empty string array")
        covered_categories.update(categories)

        if not isinstance(provenance, dict):
            raise ValidationError(f"{label}.provenance must be an object")
        for key in ("kind", "license", "description"):
            if not isinstance(provenance.get(key), str) or not provenance[key]:
                raise ValidationError(f"{label}.provenance.{key} is required")

    return covered_categories


def main() -> int:
    arguments = parse_arguments()
    repository_root = Path(__file__).resolve().parents[2]
    manifest_path = arguments.manifest or (
        repository_root / "tests/fixtures/corpus/manifest.json"
    )
    manifest_path = manifest_path.resolve()

    try:
        data = load_manifest(manifest_path)
        required, assets = validate_structure(data)
        covered = validate_assets(manifest_path.parent, assets)
    except ValidationError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    missing_categories = sorted(set(required) - covered)
    minimum_count = data["minimumAssetCount"]
    complete = (
        data["status"] == "complete"
        and len(assets) >= minimum_count
        and not missing_categories
    )
    if arguments.release and not complete:
        print(
            "ERROR: release corpus is incomplete: "
            f"status={data['status']}, assets={len(assets)}/{minimum_count}, "
            f"missing_categories={','.join(missing_categories) or 'none'}",
            file=sys.stderr,
        )
        return 1

    print(f"Verified {len(assets)} corpus asset(s) and their SHA-256 values.")
    if not complete:
        print(
            f"Scaffold remaining: {minimum_count - len(assets)} asset(s); "
            f"categories: {', '.join(missing_categories)}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
