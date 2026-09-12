#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0

"""Generate synthetic reference-scenario media and optional agent-host projects."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


class GenerationError(RuntimeError):
    """Reference fixture generation failed."""


REQUIRED_SCENARIO_IDS = (
    "synthetic-edit",
    "youtube-episode",
    "interview",
    "short-film",
)

REQUIRED_MEDIA_FIELDS = (
    "id",
    "file",
    "width",
    "height",
    "frameRate",
    "durationSeconds",
    "audioHz",
    "audioChannels",
)

SINE_FREQUENCIES_HZ = (220, 330, 440, 550, 660, 770, 880, 990, 1046, 1175)
SCENARIO_ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9-]*$")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="generated reference root (defaults to tests/fixtures/reference/generated)",
    )
    parser.add_argument(
        "--catalog",
        type=Path,
        default=None,
        help="scenario catalog JSON (defaults to tests/fixtures/reference/scenarios.json)",
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help="validate the scenario catalog schema and exit",
    )
    parser.add_argument(
        "--media-only",
        action="store_true",
        help="generate media only; skip the synthetic-edit .veproj step",
    )
    parser.add_argument(
        "--skip-project",
        action="store_true",
        help="alias for --media-only",
    )
    parser.add_argument(
        "--agent-host",
        type=Path,
        default=None,
        help="video_editor_agent_host executable (overrides env and default build path)",
    )
    return parser.parse_args()


def repository_root() -> Path:
    return Path(__file__).resolve().parents[2]


def resolve_tool(env_name: str, fallback: str) -> Path:
    configured = os.environ.get(env_name)
    located = configured or shutil.which(fallback)
    if not located:
        raise GenerationError(
            f"{fallback} is not on PATH; install ffmpeg or set {env_name}"
        )
    return Path(located)


def run_command(argv: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(argv, check=False, capture_output=True, text=True)


def require_int(
    value: Any,
    *,
    scenario_id: str,
    field: str,
    minimum: int | None = None,
) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise GenerationError(f"{scenario_id}: {field} must be an integer")
    if minimum is not None and value < minimum:
        raise GenerationError(f"{scenario_id}: {field} must be >= {minimum}")
    return value


def require_positive_number(value: Any, *, scenario_id: str, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise GenerationError(f"{scenario_id}: {field} must be a positive number")
    if value <= 0:
        raise GenerationError(f"{scenario_id}: {field} must be a positive number")
    return float(value)


def validate_media_file_name(file_name: str, scenario_id: str) -> str:
    if not isinstance(file_name, str) or not file_name.strip():
        raise GenerationError(f"{scenario_id}: media file must be a non-empty string")
    name = file_name.strip()
    candidate = Path(name)
    if candidate.is_absolute() or len(candidate.parts) != 1 or name in {".", ".."}:
        raise GenerationError(
            f"{scenario_id}: media file must be a basename without "
            f"directories or '..': {file_name}"
        )
    return candidate.name


def media_destination(scenario_dir: Path, file_name: str, scenario_id: str) -> Path:
    safe_name = validate_media_file_name(file_name, scenario_id)
    scenario_root = scenario_dir.resolve()
    destination = (scenario_root / safe_name).resolve()
    if destination.parent != scenario_root:
        raise GenerationError(
            f"{scenario_id}: media file escapes scenario directory: {file_name}"
        )
    return destination


def remove_agent_working_sidecars(root: Path) -> list[str]:
    removed: list[str] = []
    if not root.is_dir():
        return removed
    patterns = ("*.agent.working.sqlite", "*.agent.staging.sqlite")
    for pattern in patterns:
        for path in sorted(root.glob(f"*/{pattern}")):
            for candidate in (path, Path(str(path) + "-wal"), Path(str(path) + "-shm")):
                if candidate.is_file():
                    candidate.unlink()
                    removed.append(str(candidate))
    return removed


def load_catalog(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise GenerationError(f"catalog not found: {path}")
    try:
        catalog = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise GenerationError(f"catalog is not valid JSON: {exc}") from exc
    if not isinstance(catalog, dict):
        raise GenerationError("catalog root must be a JSON object")
    return catalog


def validate_catalog(catalog: dict[str, Any]) -> None:
    if catalog.get("schemaVersion") != 1:
        raise GenerationError("schemaVersion must be 1")

    scenarios = catalog.get("scenarios")
    if not isinstance(scenarios, list) or not scenarios:
        raise GenerationError("scenarios must be a non-empty array")

    seen_ids: set[str] = set()
    for scenario in scenarios:
        if not isinstance(scenario, dict):
            raise GenerationError("each scenario must be an object")
        scenario_id = scenario.get("id")
        if not isinstance(scenario_id, str) or not scenario_id:
            raise GenerationError("each scenario requires a non-empty id")
        if not SCENARIO_ID_PATTERN.fullmatch(scenario_id):
            raise GenerationError(
                f"invalid scenario id {scenario_id!r}: use lowercase letters, digits, and hyphens"
            )
        if scenario_id in seen_ids:
            raise GenerationError(f"duplicate scenario id: {scenario_id}")
        seen_ids.add(scenario_id)

        for field in ("title", "kind", "purpose", "expectedDelivery", "relinkCheck"):
            value = scenario.get(field)
            if not isinstance(value, str) or not value.strip():
                raise GenerationError(f"{scenario_id}: {field} must be a non-empty string")

        expected_edits = scenario.get("expectedEdits")
        if not isinstance(expected_edits, list) or not expected_edits:
            raise GenerationError(f"{scenario_id}: expectedEdits must be a non-empty array")
        if not all(isinstance(item, str) and item.strip() for item in expected_edits):
            raise GenerationError(f"{scenario_id}: expectedEdits entries must be non-empty strings")

        media = scenario.get("media")
        if not isinstance(media, list) or not media:
            raise GenerationError(f"{scenario_id}: media must be a non-empty array")
        seen_media_ids: set[str] = set()
        for item in media:
            if not isinstance(item, dict):
                raise GenerationError(f"{scenario_id}: each media entry must be an object")
            for field in REQUIRED_MEDIA_FIELDS:
                if field not in item:
                    raise GenerationError(f"{scenario_id}: media missing field {field}")
            media_id = item["id"]
            if not isinstance(media_id, str) or not media_id:
                raise GenerationError(f"{scenario_id}: media id must be a non-empty string")
            if media_id in seen_media_ids:
                raise GenerationError(f"{scenario_id}: duplicate media id {media_id}")
            seen_media_ids.add(media_id)
            validate_media_file_name(item["file"], scenario_id)
            require_positive_number(
                item["durationSeconds"], scenario_id=scenario_id, field="durationSeconds"
            )
            width = require_int(item["width"], scenario_id=scenario_id, field="width", minimum=0)
            height = require_int(item["height"], scenario_id=scenario_id, field="height", minimum=0)
            require_int(item["audioHz"], scenario_id=scenario_id, field="audioHz", minimum=1)
            require_int(
                item["audioChannels"], scenario_id=scenario_id, field="audioChannels", minimum=1
            )
            frame_rate = item["frameRate"]
            if not isinstance(frame_rate, str):
                raise GenerationError(f"{scenario_id}: frameRate must be a string")
            if width > 0 and height > 0:
                if not frame_rate.strip():
                    raise GenerationError(f"{scenario_id}: video media requires frameRate")
                try:
                    fps_num, fps_den = parse_frame_rate(frame_rate)
                except (TypeError, ValueError) as exc:
                    raise GenerationError(
                        f"{scenario_id}: invalid frameRate {frame_rate!r}"
                    ) from exc
                if fps_num <= 0 or fps_den <= 0:
                    raise GenerationError(f"{scenario_id}: invalid frameRate {frame_rate!r}")
            else:
                if width != 0 or height != 0:
                    raise GenerationError(
                        f"{scenario_id}: audio media must use width 0 and height 0"
                    )
                if frame_rate.strip():
                    raise GenerationError(
                        f"{scenario_id}: audio media must use an empty frameRate"
                    )

    missing = [scenario_id for scenario_id in REQUIRED_SCENARIO_IDS if scenario_id not in seen_ids]
    if missing:
        raise GenerationError(f"catalog missing required scenario ids: {', '.join(missing)}")


def scenario_by_id(catalog: dict[str, Any], scenario_id: str) -> dict[str, Any]:
    for scenario in catalog["scenarios"]:
        if scenario["id"] == scenario_id:
            return scenario
    raise GenerationError(f"scenario not found: {scenario_id}")


def sine_frequency(media_index: int) -> int:
    return SINE_FREQUENCIES_HZ[media_index % len(SINE_FREQUENCIES_HZ)]


def parse_frame_rate(frame_rate: str) -> tuple[int, int]:
    if not frame_rate:
        return (0, 0)
    if "/" in frame_rate:
        num_text, den_text = frame_rate.split("/", 1)
        return (int(num_text), int(den_text))
    return (int(float(frame_rate)), 1)


def is_video_media(media: dict[str, Any]) -> bool:
    return media["width"] > 0 and media["height"] > 0 and bool(media["frameRate"])


def generate_video(
    ffmpeg: Path,
    destination: Path,
    media: dict[str, Any],
    frequency_hz: int,
) -> None:
    width = int(media["width"])
    height = int(media["height"])
    fps_num, fps_den = parse_frame_rate(str(media["frameRate"]))
    if fps_num <= 0 or fps_den <= 0:
        raise GenerationError(f"invalid frameRate for {media['file']}: {media['frameRate']}")
    fps = f"{fps_num}/{fps_den}"
    duration = float(media["durationSeconds"])
    sample_rate = int(media["audioHz"])
    channels = int(media["audioChannels"])

    destination.parent.mkdir(parents=True, exist_ok=True)
    argv = [
        str(ffmpeg),
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-f",
        "lavfi",
        "-i",
        f"testsrc=size={width}x{height}:rate={fps}",
        "-f",
        "lavfi",
        "-i",
        f"sine=frequency={frequency_hz}:sample_rate={sample_rate}:duration={duration}",
        "-t",
        str(duration),
        "-pix_fmt",
        "yuv420p",
        "-c:v",
        "mpeg4",
        "-q:v",
        "2",
        "-c:a",
        "pcm_s16le",
        "-ac",
        str(channels),
        str(destination),
    ]
    completed = run_command(argv)
    if completed.returncode != 0 or not destination.is_file():
        raise GenerationError(
            f"ffmpeg failed for {destination.name}: {completed.stderr.strip() or completed.stdout.strip()}"
        )


def generate_audio(
    ffmpeg: Path,
    destination: Path,
    media: dict[str, Any],
    frequency_hz: int,
) -> None:
    duration = float(media["durationSeconds"])
    sample_rate = int(media["audioHz"])
    channels = int(media["audioChannels"])

    destination.parent.mkdir(parents=True, exist_ok=True)
    argv = [
        str(ffmpeg),
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-f",
        "lavfi",
        "-i",
        f"sine=frequency={frequency_hz}:sample_rate={sample_rate}:duration={duration}",
        "-t",
        str(duration),
        "-c:a",
        "pcm_s16le",
        "-ac",
        str(channels),
        str(destination),
    ]
    completed = run_command(argv)
    if completed.returncode != 0 or not destination.is_file():
        raise GenerationError(
            f"ffmpeg failed for {destination.name}: {completed.stderr.strip() or completed.stdout.strip()}"
        )


def ffprobe_summary(ffprobe: Path, path: Path) -> dict[str, Any]:
    argv = [
        str(ffprobe),
        "-hide_banner",
        "-v",
        "error",
        "-show_entries",
        "format=format_name,duration,size:stream=codec_name,codec_type,width,height,"
        "sample_rate,channels,r_frame_rate,avg_frame_rate",
        "-of",
        "json",
        str(path),
    ]
    completed = run_command(argv)
    if completed.returncode != 0:
        raise GenerationError(
            f"ffprobe failed for {path.name}: {completed.stderr.strip() or completed.stdout.strip()}"
        )
    payload = json.loads(completed.stdout)
    streams = payload.get("streams", [])
    summary: dict[str, Any] = {
        "path": str(path),
        "sizeBytes": path.stat().st_size,
        "format": payload.get("format", {}),
        "video": next((stream for stream in streams if stream.get("codec_type") == "video"), None),
        "audio": next((stream for stream in streams if stream.get("codec_type") == "audio"), None),
    }
    return summary


def write_synthetic_captions(path: Path) -> None:
    path.write_text(
        "\n".join(
            [
                "1",
                "00:00:00,000 --> 00:00:03,000",
                "Synthetic reference caption A",
                "",
                "2",
                "00:00:03,000 --> 00:00:06,000",
                "Synthetic reference caption B",
                "",
            ]
        ),
        encoding="utf-8",
    )


def resolve_agent_host(explicit: Path | None) -> Path | None:
    candidates: list[Path] = []
    if explicit is not None:
        candidates.append(explicit)
    env_value = os.environ.get("VIDEO_EDITOR_AGENT_HOST")
    if env_value:
        candidates.append(Path(env_value))
    candidates.append(
        repository_root() / "build/dev/src/agent_host/video_editor_agent_host"
    )
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    return None


def agent_session(host: Path, requests: list[dict[str, Any]]) -> list[dict[str, Any]]:
    payload = "".join(json.dumps(request, separators=(",", ":")) + "\n" for request in requests)
    completed = subprocess.run(
        [str(host)],
        input=payload,
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise GenerationError(
            f"agent host exited {completed.returncode}: {completed.stderr.strip() or completed.stdout.strip()}"
        )
    responses: list[dict[str, Any]] = []
    for line in completed.stdout.splitlines():
        if not line.strip():
            continue
        try:
            response = json.loads(line)
        except json.JSONDecodeError as exc:
            raise GenerationError(f"agent host returned invalid JSON: {line}") from exc
        if not response.get("ok"):
            error = response.get("error", {})
            code = error.get("code", "unknown")
            message = error.get("message", "agent request failed")
            raise GenerationError(f"agent host error ({code}): {message}")
        responses.append(response)
    if len(responses) != len(requests):
        raise GenerationError(
            f"agent host returned {len(responses)} responses for {len(requests)} requests"
        )
    return responses


def build_synthetic_project(
    host: Path,
    media_path: Path,
    project_path: Path,
) -> dict[str, Any]:
    project_path.parent.mkdir(parents=True, exist_ok=True)
    steps = [
        {"method": "new_project", "name": "Synthetic reference"},
        {
            "method": "insert_media",
            "mode": "ripple",
            "path": str(media_path.resolve()),
        },
        {"method": "save_project", "path": str(project_path.resolve())},
        {"method": "read_session"},
    ]
    responses = agent_session(host, steps)

    session = responses[-1]
    project = session.get("project", {})
    assets = project.get("assets", [])
    clip_count = 0
    for sequence in project.get("sequences", []):
        for track in sequence.get("tracks", []):
            clip_count += len(track.get("clips", []))

    if len(assets) != 1 or clip_count != 2:
        raise GenerationError(
            "synthetic-edit expected 1 asset and 2 linked clips "
            f"(muxed A/V), got assets={len(assets)} clips={clip_count}"
        )

    return {
        "agentHost": str(host),
        "projectPath": str(project_path.resolve()),
        "projectSizeBytes": project_path.stat().st_size if project_path.is_file() else 0,
        "assetCount": len(assets),
        "clipCount": clip_count,
        "revision": session.get("revision"),
    }


def tool_versions(ffmpeg: Path, ffprobe: Path) -> dict[str, str]:
    ffmpeg_version = run_command([str(ffmpeg), "-hide_banner", "-version"])
    ffprobe_version = run_command([str(ffprobe), "-hide_banner", "-version"])
    return {
        "ffmpeg": (ffmpeg_version.stdout.splitlines() or ["unknown"])[0].strip(),
        "ffprobe": (ffprobe_version.stdout.splitlines() or ["unknown"])[0].strip(),
    }


def generate_fixtures(
    catalog: dict[str, Any],
    output_root: Path,
    *,
    skip_project: bool,
    agent_host: Path | None,
) -> dict[str, Any]:
    ffmpeg = resolve_tool("FFMPEG", "ffmpeg")
    ffprobe = resolve_tool("FFPROBE", "ffprobe")

    run_log: dict[str, Any] = {
        "generatedAtUtc": datetime.now(timezone.utc).isoformat(),
        "catalogSchemaVersion": catalog.get("schemaVersion"),
        "tools": tool_versions(ffmpeg, ffprobe),
        "scenarios": {},
        "project": None,
    }

    media_index = 0
    overwritten: list[str] = []
    for scenario in catalog["scenarios"]:
        scenario_id = scenario["id"]
        scenario_dir = output_root / scenario_id
        scenario_dir.mkdir(parents=True, exist_ok=True)
        scenario_log: dict[str, Any] = {"media": []}

        for media in scenario["media"]:
            destination = media_destination(scenario_dir, media["file"], scenario_id)
            if destination.exists():
                overwritten.append(str(destination))
            frequency = sine_frequency(media_index)
            media_index += 1
            if is_video_media(media):
                generate_video(ffmpeg, destination, media, frequency)
            else:
                generate_audio(ffmpeg, destination, media, frequency)
            probe = ffprobe_summary(ffprobe, destination)
            probed_duration = probe.get("format", {}).get("duration")
            try:
                probed_seconds = float(probed_duration)
            except (TypeError, ValueError) as exc:
                raise GenerationError(
                    f"{destination.name}: ffprobe duration is missing or invalid"
                ) from exc
            expected_seconds = float(media["durationSeconds"])
            if abs(probed_seconds - expected_seconds) > 0.001:
                raise GenerationError(
                    f"{destination.name}: duration {probed_seconds} s does not match "
                    f"catalog {expected_seconds} s"
                )
            scenario_log["media"].append(
                {
                    "id": media["id"],
                    "file": media["file"],
                    "sineHz": frequency,
                    "probe": probe,
                }
            )

        if scenario_id == "synthetic-edit":
            captions_path = scenario_dir / "captions.srt"
            write_synthetic_captions(captions_path)
            scenario_log["captions"] = {
                "path": str(captions_path),
                "sizeBytes": captions_path.stat().st_size,
            }

        run_log["scenarios"][scenario_id] = scenario_log

    if overwritten:
        preview = ", ".join(Path(path).name for path in overwritten[:8])
        extra = "" if len(overwritten) <= 8 else f" (+{len(overwritten) - 8} more)"
        print(
            f"warning: overwriting {len(overwritten)} existing files under {output_root}: "
            f"{preview}{extra}",
            file=sys.stderr,
        )

    try:
        if skip_project:
            run_log["project"] = {"skipped": True, "reason": "--media-only/--skip-project"}
        else:
            host = resolve_agent_host(agent_host)
            if host is None:
                run_log["project"] = {
                    "skipped": True,
                    "reason": "video_editor_agent_host not found",
                }
            else:
                synthetic = scenario_by_id(catalog, "synthetic-edit")
                talking_head = media_destination(
                    output_root / "synthetic-edit",
                    synthetic["media"][0]["file"],
                    "synthetic-edit",
                )
                project_path = output_root / "synthetic-edit" / "synthetic-edit.veproj"
                run_log["project"] = build_synthetic_project(host, talking_head, project_path)
    finally:
        removed = remove_agent_working_sidecars(output_root)
        if isinstance(run_log.get("project"), dict) and removed:
            run_log["project"]["removedWorkingSidecars"] = removed

    run_log_path = output_root / "run-log.json"
    run_log_path.write_text(json.dumps(run_log, indent=2) + "\n", encoding="utf-8")
    return run_log


def main() -> int:
    args = parse_arguments()
    root = repository_root()
    catalog_path = (args.catalog or root / "tests/fixtures/reference/scenarios.json").resolve()
    catalog = load_catalog(catalog_path)

    try:
        validate_catalog(catalog)
    except GenerationError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if args.validate:
        print(f"catalog valid: {catalog_path}")
        return 0

    output_root = (args.output or root / "tests/fixtures/reference/generated").resolve()
    skip_project = args.media_only or args.skip_project
    try:
        run_log = generate_fixtures(
            catalog,
            output_root,
            skip_project=skip_project,
            agent_host=args.agent_host,
        )
    except GenerationError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(f"generated reference fixtures under {output_root}")
    print(f"run log: {output_root / 'run-log.json'}")
    if run_log.get("project", {}).get("skipped"):
        print(f"project step skipped: {run_log['project']['reason']}")
    else:
        project = run_log["project"]
        print(
            "synthetic-edit.veproj:"
            f" assets={project.get('assetCount')} clips={project.get('clipCount')}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
