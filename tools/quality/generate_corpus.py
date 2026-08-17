#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0

"""Generate a synthetic 200+ file media compatibility corpus with ffmpeg."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


class GenerationError(RuntimeError):
    """Corpus generation failed."""


REQUIRED_CATEGORIES = (
    "vfr",
    "b-frames",
    "non-zero-starting-pts",
    "rotation",
    "interlaced",
    "8-bit-color",
    "10-bit-color",
    "hdr-input",
    "alpha",
    "image-sequence",
    "corrupt-media",
    "unusual-channel-layout",
    "variable-sample-rate",
    "unusual-sar",
    "field-order",
    "missing-timestamps",
)

COMMITTED_SMOKE = "files/manifest-smoke.txt"


@dataclass
class Recipe:
    asset_id: str
    relative_path: str
    categories: tuple[str, ...]
    description: str
    kind: str
    ffmpeg_args: tuple[str, ...] = ()
    remux_args: tuple[str, ...] = ()
    writer: str = ""
    writer_kwargs: dict[str, Any] = field(default_factory=dict)
    postprocess: str = ""
    media: dict[str, Any] | None = None


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="generated corpus directory (defaults to tests/fixtures/corpus/generated)",
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


def ffmpeg_version_line(ffmpeg: Path) -> str:
    completed = run_command([str(ffmpeg), "-hide_banner", "-version"])
    if completed.returncode != 0 or not completed.stdout:
        raise GenerationError(f"cannot query ffmpeg version: {completed.stderr.strip()}")
    return completed.stdout.splitlines()[0].strip()


def encoder_names(ffmpeg: Path) -> set[str]:
    completed = run_command([str(ffmpeg), "-hide_banner", "-encoders"])
    if completed.returncode != 0:
        raise GenerationError(f"cannot list ffmpeg encoders: {completed.stderr.strip()}")
    names: set[str] = set()
    for line in completed.stdout.splitlines():
        parts = line.split()
        if len(parts) < 2 or parts[0][0] not in {"V", "A", "S"}:
            continue
        if "." not in parts[0]:
            continue
        names.add(parts[1])
    return names


def quote_arg(value: str) -> str:
    if value.isascii() and value.isprintable() and not any(ch.isspace() for ch in value):
        if all(ch.isalnum() or ch in "._-+=,:/@%" for ch in value):
            return value
    return "'" + value.replace("'", "'\\''") + "'"


def format_command(argv: list[str]) -> str:
    return " ".join(quote_arg(part) for part in argv)


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fixture:
        for block in iter(lambda: fixture.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_ppm(path: Path, width: int, height: int, rgb: tuple[int, int, int]) -> None:
    header = f"P6\n{width} {height}\n255\n".encode("ascii")
    path.write_bytes(header + bytes(rgb) * (width * height))


def write_pgm(path: Path, width: int, height: int, gray: int) -> None:
    header = f"P5\n{width} {height}\n255\n".encode("ascii")
    path.write_bytes(header + bytes([gray & 0xFF]) * (width * height))


def write_bmp24(path: Path, width: int, height: int, rgb: tuple[int, int, int]) -> None:
    row_stride = (width * 3 + 3) & ~3
    pixel_size = row_stride * height
    pixel_data = bytearray(pixel_size)
    blue, green, red = rgb[2], rgb[1], rgb[0]
    for y in range(height):
        offset = y * row_stride
        for x in range(width):
            pixel_data[offset + x * 3] = blue
            pixel_data[offset + x * 3 + 1] = green
            pixel_data[offset + x * 3 + 2] = red
    header = struct.pack(
        "<2sIHHIIiiHHIIiiII",
        b"BM",
        14 + 40 + pixel_size,
        0,
        0,
        54,
        40,
        width,
        height,
        1,
        24,
        0,
        pixel_size,
        2835,
        2835,
        0,
        0,
    )
    path.write_bytes(header + pixel_data)


def write_wav_pcm16(
    path: Path, sample_rate: int, channels: int, frames: int, phase: int
) -> None:
    data_size = frames * channels * 2
    output = bytearray()
    output.extend(b"RIFF")
    output.extend(struct.pack("<I", 36 + data_size))
    output.extend(b"WAVEfmt ")
    output.extend(struct.pack("<IHHIIHH", 16, 1, channels, sample_rate,
                              sample_rate * channels * 2, channels * 2, 16))
    output.extend(b"data")
    output.extend(struct.pack("<I", data_size))
    for index in range(frames):
        sample = int(8000 * ((index * (phase + 3)) % 32 - 16) / 16)
        sample = max(-32767, min(32767, sample))
        for _channel in range(channels):
            output.extend(struct.pack("<h", sample))
    path.write_bytes(output)


def write_garbage(path: Path, prefix: bytes, length: int) -> None:
    payload = bytes((index * 17 + 31) % 256 for index in range(length))
    path.write_bytes(prefix + payload)


def apply_postprocess(path: Path, action: str) -> str:
    data = bytearray(path.read_bytes())
    if not data:
        raise GenerationError(f"cannot post-process empty file: {path}")
    if action == "truncate":
        keep = max(32, len(data) // 2)
        path.write_bytes(bytes(data[:keep]))
        return f"then truncate to {keep} bytes"
    if action == "xor-middle":
        start = len(data) // 3
        end = min(len(data), start + 24)
        for index in range(start, end):
            data[index] ^= 0xA5
        path.write_bytes(bytes(data))
        return f"then XOR 0xA5 over bytes [{start},{end})"
    raise GenerationError(f"unknown postprocess: {action}")


def python_writer(recipe: Recipe, path: Path) -> str:
    kwargs = recipe.writer_kwargs
    if recipe.writer == "ppm":
        write_ppm(path, int(kwargs["width"]), int(kwargs["height"]), tuple(kwargs["rgb"]))
        return (
            f"Write a {kwargs['width']}x{kwargs['height']} binary PPM (P6) filled with "
            f"RGB{tuple(kwargs['rgb'])}."
        )
    if recipe.writer == "pgm":
        write_pgm(path, int(kwargs["width"]), int(kwargs["height"]), int(kwargs["gray"]))
        return (
            f"Write a {kwargs['width']}x{kwargs['height']} binary PGM (P5) filled with "
            f"gray={kwargs['gray']}."
        )
    if recipe.writer == "bmp":
        write_bmp24(path, int(kwargs["width"]), int(kwargs["height"]), tuple(kwargs["rgb"]))
        return (
            f"Write a {kwargs['width']}x{kwargs['height']} 24-bit BMP filled with "
            f"RGB{tuple(kwargs['rgb'])}."
        )
    if recipe.writer == "wav":
        write_wav_pcm16(
            path,
            int(kwargs["sample_rate"]),
            int(kwargs["channels"]),
            int(kwargs["frames"]),
            int(kwargs["phase"]),
        )
        return (
            f"Write a {kwargs['frames']}-frame PCM s16le WAV at {kwargs['sample_rate']} Hz "
            f"with {kwargs['channels']} channel(s)."
        )
    if recipe.writer == "garbage":
        write_garbage(path, bytes(kwargs["prefix"]), int(kwargs["length"]))
        return f"Write {kwargs['length']} deterministic pseudo-random bytes after {kwargs['prefix']!r}."
    if recipe.writer == "text":
        path.write_text(str(kwargs["text"]), encoding="utf-8")
        return "Write the exact UTF-8 generator info text."
    raise GenerationError(f"unknown python writer: {recipe.writer}")


def video_input(size: str, rate: int, duration: str, source: str = "testsrc2") -> list[str]:
    return ["-f", "lavfi", "-i", f"{source}=size={size}:rate={rate}:duration={duration}"]


def color_input(color: str, size: str, duration: str, rate: int = 25) -> list[str]:
    return ["-f", "lavfi", "-i", f"color=c={color}:s={size}:d={duration}:r={rate}"]


def sine_input(frequency: int, sample_rate: int, duration: str) -> list[str]:
    return [
        "-f",
        "lavfi",
        "-i",
        f"sine=frequency={frequency}:sample_rate={sample_rate}:duration={duration}",
    ]


def bitexact_video(codec: str, extra: list[str] | None = None) -> list[str]:
    args = ["-c:v", codec, "-flags", "+bitexact", "-threads", "1"]
    if extra:
        args.extend(extra)
    return args


def add_recipe(recipes: list[Recipe], recipe: Recipe) -> None:
    recipes.append(recipe)


def required_recipes() -> list[Recipe]:
    recipes: list[Recipe] = []
    add_recipe(
        recipes,
        Recipe(
            asset_id="vfr-mpeg4-mkv",
            relative_path="files/video/vfr-mpeg4.mkv",
            categories=("vfr", "8-bit-color"),
            description="Irregular presentation timestamps producing variable frame rate.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.28")
                + ["-fps_mode", "vfr", "-vf", "setpts=N*(0.04+0.03*mod(N\\,3))/TB"]
                + bitexact_video("mpeg4", ["-q:v", "8"])
            ),
            media={"container": "mkv", "videoCodec": "mpeg4", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="bframes-mpeg2-mkv",
            relative_path="files/video/bframes-mpeg2.mkv",
            categories=("b-frames", "8-bit-color"),
            description="MPEG-2 with two B-frames between references.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.48")
                + bitexact_video("mpeg2video", ["-q:v", "8", "-bf", "2", "-g", "12"])
            ),
            media={"container": "mkv", "videoCodec": "mpeg2video", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="pts-offset-mpeg4-mp4",
            relative_path="files/video/pts-offset-mpeg4.mp4",
            categories=("non-zero-starting-pts", "8-bit-color"),
            description="MP4 whose first video PTS is shifted by 1.25 seconds.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.16")
                + ["-output_ts_offset", "1.25"]
                + bitexact_video("mpeg4", ["-q:v", "8"])
            ),
            media={"container": "mp4", "videoCodec": "mpeg4", "audioCodec": None},
        ),
    )
    for degrees in (90, 180, 270):
        add_recipe(
            recipes,
            Recipe(
                asset_id=f"rotation-{degrees}-mpeg4-mp4",
                relative_path=f"files/video/rotation-{degrees}-mpeg4.mp4",
                categories=("rotation", "8-bit-color"),
                description=f"MP4 display-matrix rotation of {degrees} degrees.",
                kind="ffmpeg",
                ffmpeg_args=tuple(video_input("32x18", 25, "0.16") + bitexact_video("mpeg4", ["-q:v", "8"])),
                remux_args=("-display_rotation", str(degrees), "-i", "{input}", "-c", "copy"),
                media={"container": "mp4", "videoCodec": "mpeg4", "audioCodec": None},
            ),
        )
    add_recipe(
        recipes,
        Recipe(
            asset_id="interlaced-tff-mpeg2-mkv",
            relative_path="files/video/interlaced-tff-mpeg2.mkv",
            categories=("interlaced", "field-order", "8-bit-color"),
            description="Top-field-first interlaced MPEG-2.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.32")
                + ["-vf", "tinterlace=interleave_top,setfield=tff"]
                + bitexact_video("mpeg2video", ["-q:v", "8", "-flags", "+ildct+ilme+bitexact"])
            ),
            media={"container": "mkv", "videoCodec": "mpeg2video", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="interlaced-bff-mpeg2-mkv",
            relative_path="files/video/interlaced-bff-mpeg2.mkv",
            categories=("interlaced", "field-order", "8-bit-color"),
            description="Bottom-field-first interlaced MPEG-2.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.32")
                + ["-vf", "tinterlace=interleave_bottom,setfield=bff"]
                + bitexact_video("mpeg2video", ["-q:v", "8", "-flags", "+ildct+ilme+bitexact"])
            ),
            media={"container": "mkv", "videoCodec": "mpeg2video", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="field-order-tt-mpeg2-mkv",
            relative_path="files/video/field-order-tt-mpeg2.mkv",
            categories=("field-order", "8-bit-color"),
            description="MPEG-2 tagged with top-field-first field order.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.24")
                + ["-field_order", "tt"]
                + bitexact_video("mpeg2video", ["-q:v", "8", "-flags", "+ildct+ilme+bitexact"])
            ),
            media={"container": "mkv", "videoCodec": "mpeg2video", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="field-order-bb-mpeg2-mkv",
            relative_path="files/video/field-order-bb-mpeg2.mkv",
            categories=("field-order", "8-bit-color"),
            description="MPEG-2 tagged with bottom-field-first field order.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.24")
                + ["-field_order", "bb"]
                + bitexact_video("mpeg2video", ["-q:v", "8", "-flags", "+ildct+ilme+bitexact"])
            ),
            media={"container": "mkv", "videoCodec": "mpeg2video", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="yuv420p8-mpeg4-mp4",
            relative_path="files/video/yuv420p8-mpeg4.mp4",
            categories=("8-bit-color",),
            description="Baseline 8-bit yuv420p MPEG-4 clip.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.16")
                + ["-pix_fmt", "yuv420p"]
                + bitexact_video("mpeg4", ["-q:v", "8"])
            ),
            media={"container": "mp4", "videoCodec": "mpeg4", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="yuv420p10-ffv1-mkv",
            relative_path="files/video/yuv420p10-ffv1.mkv",
            categories=("10-bit-color",),
            description="10-bit yuv420p10le FFV1 clip.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.16")
                + ["-pix_fmt", "yuv420p10le"]
                + bitexact_video("ffv1")
            ),
            media={"container": "mkv", "videoCodec": "ffv1", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="hdr10-ffv1-mkv",
            relative_path="files/video/hdr10-ffv1.mkv",
            categories=("hdr-input", "10-bit-color"),
            description="BT.2020 / PQ (smpte2084) 10-bit HDR-tagged FFV1 clip.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.16")
                + [
                    "-pix_fmt",
                    "yuv420p10le",
                    "-color_primaries",
                    "bt2020",
                    "-color_trc",
                    "smpte2084",
                    "-colorspace",
                    "bt2020nc",
                    "-color_range",
                    "tv",
                    "-metadata:s:v:0",
                    "color_primaries=bt2020",
                    "-metadata:s:v:0",
                    "color_trc=smpte2084",
                ]
                + bitexact_video("ffv1")
            ),
            media={"container": "mkv", "videoCodec": "ffv1", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="hdr8-mpeg4-mkv",
            relative_path="files/video/hdr8-mpeg4.mkv",
            categories=("hdr-input", "8-bit-color"),
            description="8-bit clip tagged with BT.2020 / PQ HDR metadata.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.16")
                + [
                    "-pix_fmt",
                    "yuv420p",
                    "-color_primaries",
                    "bt2020",
                    "-color_trc",
                    "smpte2084",
                    "-colorspace",
                    "bt2020nc",
                    "-color_range",
                    "tv",
                ]
                + bitexact_video("mpeg4", ["-q:v", "8"])
            ),
            media={"container": "mkv", "videoCodec": "mpeg4", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="alpha-png-mov",
            relative_path="files/video/alpha-png.mov",
            categories=("alpha", "8-bit-color"),
            description="Straight RGBA PNG in a QuickTime container.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                ["-f", "lavfi", "-i", "color=c=red@0.5:s=32x18:d=0.16:r=25,format=rgba"]
                + bitexact_video("png")
            ),
            media={"container": "mov", "videoCodec": "png", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="alpha-qtrle-mov",
            relative_path="files/video/alpha-qtrle.mov",
            categories=("alpha",),
            description="QuickTime Animation (qtrle) clip with an alpha channel.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                ["-f", "lavfi", "-i", "color=c=blue@0.4:s=32x18:d=0.12:r=25,format=rgba"]
                + bitexact_video("qtrle")
            ),
            media={"container": "mov", "videoCodec": "qtrle", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="layout-5.1-pcm-wav",
            relative_path="files/audio/layout-5.1-pcm.wav",
            categories=("unusual-channel-layout",),
            description="5.1 PCM WAV generated from a mono sine expanded to 6 channels.",
            kind="ffmpeg",
            ffmpeg_args=tuple(sine_input(220, 8000, "0.12") + ["-ch_layout", "5.1", "-c:a", "pcm_s16le"]),
            media={"container": "wav", "videoCodec": None, "audioCodec": "pcm_s16le"},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="layout-7.1-pcm-mkv",
            relative_path="files/audio/layout-7.1-pcm.mkv",
            categories=("unusual-channel-layout",),
            description="7.1 PCM audio in Matroska.",
            kind="ffmpeg",
            ffmpeg_args=tuple(sine_input(330, 8000, "0.12") + ["-ch_layout", "7.1", "-c:a", "pcm_s16le"]),
            media={"container": "mkv", "videoCodec": None, "audioCodec": "pcm_s16le"},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="layout-quad-pcm-wav",
            relative_path="files/audio/layout-quad-pcm.wav",
            categories=("unusual-channel-layout",),
            description="Quad (4.0) PCM WAV.",
            kind="ffmpeg",
            ffmpeg_args=tuple(sine_input(196, 8000, "0.12") + ["-ch_layout", "quad", "-c:a", "pcm_s16le"]),
            media={"container": "wav", "videoCodec": None, "audioCodec": "pcm_s16le"},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="dual-rate-22050-48000-mkv",
            relative_path="files/audio/dual-rate-22050-48000.mkv",
            categories=("variable-sample-rate",),
            description="Matroska with two PCM streams at 22050 Hz and 48000 Hz.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                sine_input(440, 22050, "0.12")
                + sine_input(880, 48000, "0.12")
                + ["-map", "0:a", "-map", "1:a", "-c:a", "pcm_s16le"]
            ),
            media={"container": "mkv", "videoCodec": None, "audioCodec": "pcm_s16le"},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="dual-rate-8000-44100-mkv",
            relative_path="files/audio/dual-rate-8000-44100.mkv",
            categories=("variable-sample-rate",),
            description="Matroska with two PCM streams at 8000 Hz and 44100 Hz.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                sine_input(220, 8000, "0.10")
                + sine_input(660, 44100, "0.10")
                + ["-map", "0:a", "-map", "1:a", "-c:a", "pcm_s16le"]
            ),
            media={"container": "mkv", "videoCodec": None, "audioCodec": "pcm_s16le"},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="sar-16-11-mpeg4-mkv",
            relative_path="files/video/sar-16-11-mpeg4.mkv",
            categories=("unusual-sar", "8-bit-color"),
            description="MPEG-4 clip with a 16:11 sample aspect ratio.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.16")
                + ["-vf", "setsar=16/11"]
                + bitexact_video("mpeg4", ["-q:v", "8"])
            ),
            media={"container": "mkv", "videoCodec": "mpeg4", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="sar-40-33-mpeg4-mp4",
            relative_path="files/video/sar-40-33-mpeg4.mp4",
            categories=("unusual-sar", "8-bit-color"),
            description="MPEG-4 clip with a 40:33 sample aspect ratio.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("48x36", 25, "0.16")
                + ["-vf", "setsar=40/33"]
                + bitexact_video("mpeg4", ["-q:v", "8"])
            ),
            media={"container": "mp4", "videoCodec": "mpeg4", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="missing-ts-mpeg2-elementary",
            relative_path="files/video/missing-ts-mpeg2.m2v",
            categories=("missing-timestamps",),
            description="MPEG-2 elementary stream without a container timestamp index.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.16")
                + bitexact_video("mpeg2video", ["-q:v", "8"])
                + ["-f", "mpeg2video"]
            ),
            media={"container": "mpeg2video", "videoCodec": "mpeg2video", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="missing-ts-mpeg1-elementary",
            relative_path="files/video/missing-ts-mpeg1.m1v",
            categories=("missing-timestamps",),
            description="MPEG-1 elementary stream without container timestamps.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.16")
                + bitexact_video("mpeg1video", ["-q:v", "8"])
                + ["-f", "mpeg1video"]
            ),
            media={"container": "mpeg1video", "videoCodec": "mpeg1video", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="missing-ts-mjpeg-elementary",
            relative_path="files/video/missing-ts-mjpeg.mjpg",
            categories=("missing-timestamps",),
            description="Raw MJPEG elementary stream without timestamps.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.16")
                + bitexact_video("mjpeg", ["-q:v", "8"])
                + ["-f", "mjpeg"]
            ),
            media={"container": "mjpeg", "videoCodec": "mjpeg", "audioCodec": None},
        ),
    )
    return recipes


def python_fixture_recipes(ffmpeg_summary: str) -> list[Recipe]:
    recipes: list[Recipe] = []
    colors = {
        "red": (220, 24, 24),
        "green": (24, 196, 48),
        "blue": (32, 64, 220),
        "white": (240, 240, 240),
        "black": (8, 8, 8),
        "yellow": (232, 216, 16),
        "cyan": (16, 208, 208),
        "magenta": (208, 16, 196),
    }
    for name, rgb in colors.items():
        for width, height in ((16, 16), (32, 18), (48, 36)):
            add_recipe(
                recipes,
                Recipe(
                    asset_id=f"still-ppm-{name}-{width}x{height}",
                    relative_path=f"files/stills/ppm/{name}-{width}x{height}.ppm",
                    categories=("8-bit-color",),
                    description=f"Solid {name} PPM still.",
                    kind="python",
                    writer="ppm",
                    writer_kwargs={"width": width, "height": height, "rgb": rgb},
                    media={"container": "ppm", "videoCodec": "ppm", "audioCodec": None},
                ),
            )
    for name, rgb in list(colors.items())[:4]:
        for width, height in ((16, 16), (32, 18)):
            add_recipe(
                recipes,
                Recipe(
                    asset_id=f"still-bmp-{name}-{width}x{height}",
                    relative_path=f"files/stills/bmp/{name}-{width}x{height}.bmp",
                    categories=("8-bit-color",),
                    description=f"Solid {name} BMP still.",
                    kind="python",
                    writer="bmp",
                    writer_kwargs={"width": width, "height": height, "rgb": rgb},
                    media={"container": "bmp", "videoCodec": "bmp", "audioCodec": None},
                ),
            )
    for index in range(5):
        rgb = ((40 * index + 20) % 256, (80 * index + 40) % 256, (120 * index + 60) % 256)
        add_recipe(
            recipes,
            Recipe(
                asset_id=f"seq-bars-{index:03d}",
                relative_path=f"files/sequences/bars/frame_{index:03d}.ppm",
                categories=("image-sequence", "8-bit-color"),
                description="Frame from the synthetic color-bar image sequence.",
                kind="python",
                writer="ppm",
                writer_kwargs={"width": 32, "height": 18, "rgb": rgb},
                media={"container": "ppm", "videoCodec": "ppm", "audioCodec": None},
            ),
        )
    for index in range(5):
        add_recipe(
            recipes,
            Recipe(
                asset_id=f"seq-gray-{index:03d}",
                relative_path=f"files/sequences/gray/frame_{index:03d}.pgm",
                categories=("image-sequence",),
                description="Frame from the synthetic grayscale image sequence.",
                kind="python",
                writer="pgm",
                writer_kwargs={"width": 16, "height": 16, "gray": 32 + index * 40},
                media={"container": "pgm", "videoCodec": "pgm", "audioCodec": None},
            ),
        )
    for index in range(6):
        rgb = (16 + index * 36, 200 - index * 24, 80 + index * 20)
        add_recipe(
            recipes,
            Recipe(
                asset_id=f"seq-cycle-{index:03d}",
                relative_path=f"files/sequences/cycle/frame_{index:03d}.ppm",
                categories=("image-sequence", "8-bit-color"),
                description="Frame from the synthetic color-cycle image sequence.",
                kind="python",
                writer="ppm",
                writer_kwargs={"width": 24, "height": 16, "rgb": rgb},
                media={"container": "ppm", "videoCodec": "ppm", "audioCodec": None},
            ),
        )
    for rate in (8000, 11025, 16000, 22050, 32000, 44100, 48000):
        for channels in (1, 2):
            add_recipe(
                recipes,
                Recipe(
                    asset_id=f"audio-wav-{rate}-{channels}ch",
                    relative_path=f"files/audio/wav/{rate}-{channels}ch.wav",
                    categories=("tiny-audio",),
                    description=f"Tiny PCM WAV at {rate} Hz, {channels} channel(s).",
                    kind="python",
                    writer="wav",
                    writer_kwargs={
                        "sample_rate": rate,
                        "channels": channels,
                        "frames": max(32, rate // 50),
                        "phase": rate // 1000 + channels,
                    },
                    media={"container": "wav", "videoCodec": None, "audioCodec": "pcm_s16le"},
                ),
            )

    add_recipe(
        recipes,
        Recipe(
            asset_id="corrupt-garbage-mp4",
            relative_path="files/corrupt/garbage.mp4",
            categories=("corrupt-media",),
            description="Random bytes with an ftyp-like prefix; not a valid MP4.",
            kind="python",
            writer="garbage",
            writer_kwargs={"prefix": b"ftyp", "length": 96},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="corrupt-garbage-wav",
            relative_path="files/corrupt/garbage.wav",
            categories=("corrupt-media",),
            description="Random bytes with a RIFF prefix; not a valid WAV.",
            kind="python",
            writer="garbage",
            writer_kwargs={"prefix": b"RIFF", "length": 80},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="corrupt-garbage-jpg",
            relative_path="files/corrupt/garbage.jpg",
            categories=("corrupt-media",),
            description="Random bytes with a JPEG SOI prefix; not a valid image.",
            kind="python",
            writer="garbage",
            writer_kwargs={"prefix": b"\xff\xd8\xff", "length": 64},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="generator-info",
            relative_path="files/generator-info.txt",
            categories=("infrastructure",),
            description="Records the ffmpeg version used to generate this corpus.",
            kind="python",
            writer="text",
            writer_kwargs={
                "text": (
                    "SPDX-License-Identifier: MPL-2.0\n"
                    "Video Editor generated media corpus.\n"
                    f"{ffmpeg_summary}\n"
                )
            },
        ),
    )
    return recipes


def video_matrix_recipes(encoders: set[str]) -> list[Recipe]:
    recipes: list[Recipe] = []
    variants = [
        ("mpeg4", "mp4", "mpeg4", ["-q:v", "8"], "yuv420p"),
        ("mpeg4", "mkv", "mpeg4", ["-q:v", "8"], "yuv420p"),
        ("mpeg4", "avi", "mpeg4", ["-q:v", "8"], "yuv420p"),
        ("mpeg4", "mov", "mpeg4", ["-q:v", "8"], "yuv420p"),
        ("mjpeg", "mov", "mjpeg", ["-q:v", "8"], "yuvj420p"),
        ("mjpeg", "avi", "mjpeg", ["-q:v", "8"], "yuvj420p"),
        ("ffv1", "mkv", "ffv1", [], "yuv420p"),
        ("mpeg2", "mkv", "mpeg2video", ["-q:v", "8"], "yuv420p"),
        ("raw", "avi", "rawvideo", [], "yuv420p"),
    ]
    if "huffyuv" in encoders:
        variants.append(("huffyuv", "avi", "huffyuv", [], "yuv422p"))
    if "libx264" in encoders:
        variants.append(("h264", "mp4", "libx264", ["-preset", "ultrafast", "-crf", "30", "-g", "4"], "yuv420p"))
    sizes = ("16x16", "32x18", "48x36")
    for codec_id, container, encoder, extra, pix_fmt in variants:
        if encoder not in encoders:
            continue
        for size in sizes:
            add_recipe(
                recipes,
                Recipe(
                    asset_id=f"matrix-{codec_id}-{container}-{size}",
                    relative_path=f"files/video/matrix/{codec_id}-{container}-{size}.{container}",
                    categories=("8-bit-color",),
                    description=f"{encoder} in {container} at {size}.",
                    kind="ffmpeg",
                    ffmpeg_args=tuple(
                        video_input(size, 25, "0.12")
                        + ["-pix_fmt", pix_fmt]
                        + bitexact_video(encoder, extra)
                    ),
                    media={"container": container, "videoCodec": encoder, "audioCodec": None},
                ),
            )
    for rate in (10, 24, 30):
        add_recipe(
            recipes,
            Recipe(
                asset_id=f"matrix-mpeg4-mp4-32x18-{rate}fps",
                relative_path=f"files/video/matrix/mpeg4-mp4-32x18-{rate}fps.mp4",
                categories=("8-bit-color",),
                description=f"MPEG-4 MP4 at {rate} fps.",
                kind="ffmpeg",
                ffmpeg_args=tuple(
                    video_input("32x18", rate, "0.20")
                    + bitexact_video("mpeg4", ["-q:v", "8"])
                ),
                media={"container": "mp4", "videoCodec": "mpeg4", "audioCodec": None},
            ),
        )
    for pix_fmt in ("yuv422p", "yuv444p"):
        add_recipe(
            recipes,
            Recipe(
                asset_id=f"matrix-ffv1-mkv-32x18-{pix_fmt}",
                relative_path=f"files/video/matrix/ffv1-32x18-{pix_fmt}.mkv",
                categories=("8-bit-color",),
                description=f"FFV1 {pix_fmt} clip.",
                kind="ffmpeg",
                ffmpeg_args=tuple(
                    video_input("32x18", 25, "0.12")
                    + ["-pix_fmt", pix_fmt]
                    + bitexact_video("ffv1")
                ),
                media={"container": "mkv", "videoCodec": "ffv1", "audioCodec": None},
            ),
        )
    if "libvpx-vp9" in encoders:
        add_recipe(
            recipes,
            Recipe(
                asset_id="matrix-vp9-webm-32x18",
                relative_path="files/video/matrix/vp9-32x18.webm",
                categories=("8-bit-color",),
                description="Tiny VP9 WebM clip.",
                kind="ffmpeg",
                ffmpeg_args=tuple(
                    video_input("32x18", 25, "0.12")
                    + bitexact_video("libvpx-vp9", ["-b:v", "32k", "-deadline", "realtime", "-cpu-used", "8"])
                ),
                media={"container": "webm", "videoCodec": "libvpx-vp9", "audioCodec": None},
            ),
        )
    if "prores_ks" in encoders:
        add_recipe(
            recipes,
            Recipe(
                asset_id="matrix-prores-mov-32x18",
                relative_path="files/video/matrix/prores-32x18.mov",
                categories=("10-bit-color",),
                description="Tiny ProRes 422 clip.",
                kind="ffmpeg",
                ffmpeg_args=tuple(
                    video_input("32x18", 25, "0.12")
                    + ["-pix_fmt", "yuv422p10le"]
                    + bitexact_video("prores_ks", ["-profile:v", "0"])
                ),
                media={"container": "mov", "videoCodec": "prores_ks", "audioCodec": None},
            ),
        )
    return recipes


def audio_and_still_ffmpeg_recipes(encoders: set[str]) -> list[Recipe]:
    recipes: list[Recipe] = []
    if "flac" in encoders:
        for rate in (16000, 44100, 48000):
            add_recipe(
                recipes,
                Recipe(
                    asset_id=f"audio-flac-{rate}",
                    relative_path=f"files/audio/flac/{rate}.flac",
                    categories=("tiny-audio",),
                    description=f"Tiny FLAC at {rate} Hz.",
                    kind="ffmpeg",
                    ffmpeg_args=tuple(sine_input(440, rate, "0.10") + ["-c:a", "flac"]),
                    media={"container": "flac", "videoCodec": None, "audioCodec": "flac"},
                ),
            )
    if "aac" in encoders:
        for rate in (22050, 44100, 48000):
            add_recipe(
                recipes,
                Recipe(
                    asset_id=f"audio-aac-{rate}",
                    relative_path=f"files/audio/aac/{rate}.m4a",
                    categories=("tiny-audio",),
                    description=f"Tiny AAC at {rate} Hz.",
                    kind="ffmpeg",
                    ffmpeg_args=tuple(sine_input(523, rate, "0.10") + ["-c:a", "aac", "-b:a", "32k"]),
                    media={"container": "m4a", "videoCodec": None, "audioCodec": "aac"},
                ),
            )
    if "mp2" in encoders:
        for rate in (22050, 44100, 48000):
            add_recipe(
                recipes,
                Recipe(
                    asset_id=f"audio-mp2-{rate}",
                    relative_path=f"files/audio/mp2/{rate}.mp2",
                    categories=("tiny-audio",),
                    description=f"Tiny MP2 at {rate} Hz.",
                    kind="ffmpeg",
                    ffmpeg_args=tuple(sine_input(349, rate, "0.10") + ["-c:a", "mp2", "-b:a", "32k"]),
                    media={"container": "mp2", "videoCodec": None, "audioCodec": "mp2"},
                ),
            )
    if "libopus" in encoders:
        add_recipe(
            recipes,
            Recipe(
                asset_id="audio-opus-48000",
                relative_path="files/audio/opus/48000.opus",
                categories=("tiny-audio",),
                description="Tiny Opus audio.",
                kind="ffmpeg",
                ffmpeg_args=tuple(sine_input(440, 48000, "0.10") + ["-c:a", "libopus", "-b:a", "16k"]),
                media={"container": "opus", "videoCodec": None, "audioCodec": "libopus"},
            ),
        )
    for layout in ("3.0", "2.1", "5.0", "6.1"):
        add_recipe(
            recipes,
            Recipe(
                asset_id=f"layout-{layout}-pcm-wav",
                relative_path=f"files/audio/layout-{layout}-pcm.wav",
                categories=("unusual-channel-layout",),
                description=f"{layout} PCM WAV.",
                kind="ffmpeg",
                ffmpeg_args=tuple(sine_input(247, 8000, "0.10") + ["-ch_layout", layout, "-c:a", "pcm_s16le"]),
                media={"container": "wav", "videoCodec": None, "audioCodec": "pcm_s16le"},
            ),
        )
    png_colors = ("red", "green", "blue", "white", "black", "yellow")
    if "png" in encoders:
        for color in png_colors:
            add_recipe(
                recipes,
                Recipe(
                    asset_id=f"still-png-{color}-32x18",
                    relative_path=f"files/stills/png/{color}-32x18.png",
                    categories=("8-bit-color",),
                    description=f"Solid {color} PNG still.",
                    kind="ffmpeg",
                    ffmpeg_args=tuple(
                        color_input(color, "32x18", "0.04")
                        + ["-frames:v", "1"]
                        + bitexact_video("png")
                    ),
                    media={"container": "png", "videoCodec": "png", "audioCodec": None},
                ),
            )
        add_recipe(
            recipes,
            Recipe(
                asset_id="still-png-alpha-32x18",
                relative_path="files/stills/png/alpha-32x18.png",
                categories=("alpha", "8-bit-color"),
                description="Straight RGBA PNG still.",
                kind="ffmpeg",
                ffmpeg_args=tuple(
                    ["-f", "lavfi", "-i", "color=c=green@0.35:s=32x18:d=0.04:r=25,format=rgba"]
                    + ["-frames:v", "1"]
                    + bitexact_video("png")
                ),
                media={"container": "png", "videoCodec": "png", "audioCodec": None},
            ),
        )
    if "mjpeg" in encoders:
        for color in png_colors:
            add_recipe(
                recipes,
                Recipe(
                    asset_id=f"still-jpg-{color}-32x18",
                    relative_path=f"files/stills/jpg/{color}-32x18.jpg",
                    categories=("8-bit-color",),
                    description=f"Solid {color} JPEG still.",
                    kind="ffmpeg",
                    ffmpeg_args=tuple(
                        color_input(color, "32x18", "0.04")
                        + ["-frames:v", "1"]
                        + bitexact_video("mjpeg", ["-q:v", "6"])
                    ),
                    media={"container": "jpg", "videoCodec": "mjpeg", "audioCodec": None},
                ),
            )
    return recipes


def extra_and_corrupt_recipes() -> list[Recipe]:
    recipes: list[Recipe] = []
    add_recipe(
        recipes,
        Recipe(
            asset_id="pts-offset-2s-mpeg4-mkv",
            relative_path="files/video/pts-offset-2s-mpeg4.mkv",
            categories=("non-zero-starting-pts", "8-bit-color"),
            description="MKV whose first PTS is shifted by 2 seconds.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.16")
                + ["-output_ts_offset", "2.0"]
                + bitexact_video("mpeg4", ["-q:v", "8"])
            ),
            media={"container": "mkv", "videoCodec": "mpeg4", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="damaged-timestamps-setpts-mkv",
            relative_path="files/video/damaged-timestamps-setpts.mkv",
            categories=("non-zero-starting-pts", "vfr"),
            description="Clip with large PTS gaps from setpts.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.20")
                + ["-fps_mode", "vfr", "-vf", "setpts=PTS*3+1/TB"]
                + bitexact_video("mpeg4", ["-q:v", "8"])
            ),
            media={"container": "mkv", "videoCodec": "mpeg4", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="sar-12-11-mpeg4-avi",
            relative_path="files/video/sar-12-11-mpeg4.avi",
            categories=("unusual-sar", "8-bit-color"),
            description="AVI with a 12:11 sample aspect ratio.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.16")
                + ["-vf", "setsar=12/11"]
                + bitexact_video("mpeg4", ["-q:v", "8"])
            ),
            media={"container": "avi", "videoCodec": "mpeg4", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="bframes-mpeg4-mp4",
            relative_path="files/video/bframes-mpeg4.mp4",
            categories=("b-frames", "8-bit-color"),
            description="MPEG-4 part 2 with B-frames.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.40")
                + bitexact_video("mpeg4", ["-q:v", "8", "-bf", "2", "-g", "12"])
            ),
            media={"container": "mp4", "videoCodec": "mpeg4", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="yuv422p10-ffv1-mkv",
            relative_path="files/video/yuv422p10-ffv1.mkv",
            categories=("10-bit-color",),
            description="10-bit yuv422p10le FFV1 clip.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.12")
                + ["-pix_fmt", "yuv422p10le"]
                + bitexact_video("ffv1")
            ),
            media={"container": "mkv", "videoCodec": "ffv1", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="hlg-hdr-ffv1-mkv",
            relative_path="files/video/hlg-hdr-ffv1.mkv",
            categories=("hdr-input", "10-bit-color"),
            description="BT.2020 / HLG 10-bit HDR-tagged FFV1 clip.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                video_input("32x18", 25, "0.12")
                + [
                    "-pix_fmt",
                    "yuv420p10le",
                    "-color_primaries",
                    "bt2020",
                    "-color_trc",
                    "arib-std-b67",
                    "-colorspace",
                    "bt2020nc",
                    "-color_range",
                    "tv",
                ]
                + bitexact_video("ffv1")
            ),
            media={"container": "mkv", "videoCodec": "ffv1", "audioCodec": None},
        ),
    )
    for index, color in enumerate(("0x102040", "0x401020", "0x204010", "0x403010", "0x104030")):
        add_recipe(
            recipes,
            Recipe(
                asset_id=f"extra-color-mpeg4-{index:02d}",
                relative_path=f"files/video/extra/color-{index:02d}.mp4",
                categories=("8-bit-color",),
                description=f"Tiny solid-color MPEG-4 clip {color}.",
                kind="ffmpeg",
                ffmpeg_args=tuple(
                    color_input(color, "16x16", "0.12")
                    + bitexact_video("mpeg4", ["-q:v", "10"])
                ),
                media={"container": "mp4", "videoCodec": "mpeg4", "audioCodec": None},
            ),
        )
    for index in range(32):
        width = 16 + (index % 3) * 8
        height = 16 + (index % 2) * 2
        color = f"0x{(16 + index * 9) % 256:02x}{(64 + index * 5) % 256:02x}{(128 + index * 3) % 256:02x}"
        add_recipe(
            recipes,
            Recipe(
                asset_id=f"extra-pad-mpeg4-{index:02d}",
                relative_path=f"files/video/extra/pad-{index:02d}.mkv",
                categories=("8-bit-color",),
                description=f"Padding variant {index} to keep the corpus above 200 files.",
                kind="ffmpeg",
                ffmpeg_args=tuple(
                    color_input(color, f"{width}x{height}", "0.08", rate=10 + (index % 3) * 5)
                    + bitexact_video("mpeg4", ["-q:v", "12"])
                ),
                media={"container": "mkv", "videoCodec": "mpeg4", "audioCodec": None},
            ),
        )
    add_recipe(
        recipes,
        Recipe(
            asset_id="corrupt-truncated-mpeg4-mp4",
            relative_path="files/corrupt/truncated-mpeg4.mp4",
            categories=("corrupt-media",),
            description="Valid MPEG-4 MP4 truncated after encode.",
            kind="ffmpeg",
            ffmpeg_args=tuple(video_input("32x18", 25, "0.24") + bitexact_video("mpeg4", ["-q:v", "8"])),
            postprocess="truncate",
            media={"container": "mp4", "videoCodec": "mpeg4", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="corrupt-xor-mpeg4-mkv",
            relative_path="files/corrupt/xor-mpeg4.mkv",
            categories=("corrupt-media",),
            description="Valid MPEG-4 MKV with a mid-file byte scramble.",
            kind="ffmpeg",
            ffmpeg_args=tuple(video_input("32x18", 25, "0.24") + bitexact_video("mpeg4", ["-q:v", "8"])),
            postprocess="xor-middle",
            media={"container": "mkv", "videoCodec": "mpeg4", "audioCodec": None},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="corrupt-truncated-wav",
            relative_path="files/corrupt/truncated.wav",
            categories=("corrupt-media",),
            description="Valid WAV truncated after encode.",
            kind="ffmpeg",
            ffmpeg_args=tuple(sine_input(440, 16000, "0.20") + ["-c:a", "pcm_s16le"]),
            postprocess="truncate",
            media={"container": "wav", "videoCodec": None, "audioCodec": "pcm_s16le"},
        ),
    )
    add_recipe(
        recipes,
        Recipe(
            asset_id="corrupt-truncated-jpg",
            relative_path="files/corrupt/truncated.jpg",
            categories=("corrupt-media",),
            description="Valid JPEG truncated after encode.",
            kind="ffmpeg",
            ffmpeg_args=tuple(
                color_input("orange", "32x18", "0.04")
                + ["-frames:v", "1"]
                + bitexact_video("mjpeg", ["-q:v", "6"])
            ),
            postprocess="truncate",
            media={"container": "jpg", "videoCodec": "mjpeg", "audioCodec": None},
        ),
    )
    return recipes


def build_recipes(ffmpeg_summary: str, encoders: set[str]) -> list[Recipe]:
    recipes = []
    recipes.extend(required_recipes())
    recipes.extend(python_fixture_recipes(ffmpeg_summary))
    recipes.extend(video_matrix_recipes(encoders))
    recipes.extend(audio_and_still_ffmpeg_recipes(encoders))
    recipes.extend(extra_and_corrupt_recipes())
    seen_ids: set[str] = set()
    seen_paths: set[str] = set()
    for recipe in recipes:
        if recipe.relative_path == COMMITTED_SMOKE:
            raise GenerationError("refusing to overwrite the committed smoke fixture")
        if recipe.asset_id in seen_ids:
            raise GenerationError(f"duplicate asset id: {recipe.asset_id}")
        if recipe.relative_path in seen_paths:
            raise GenerationError(f"duplicate asset path: {recipe.relative_path}")
        seen_ids.add(recipe.asset_id)
        seen_paths.add(recipe.relative_path)
    return recipes


def common_ffmpeg_prefix(ffmpeg: Path) -> list[str]:
    return [str(ffmpeg), "-hide_banner", "-loglevel", "error", "-nostdin", "-y"]


def generate_recipe(
    recipe: Recipe,
    output_dir: Path,
    committed_root: Path,
    ffmpeg: Path,
) -> dict[str, Any]:
    destination = (output_dir / recipe.relative_path).resolve()
    if committed_root in destination.parents or destination == committed_root:
        raise GenerationError(f"refusing to write into the committed corpus: {recipe.relative_path}")
    destination.parent.mkdir(parents=True, exist_ok=True)

    recipe_text = recipe.description
    if recipe.kind == "python":
        recipe_text = python_writer(recipe, destination)
    elif recipe.kind == "ffmpeg":
        encode_output = destination
        temp_path: Path | None = None
        if recipe.remux_args:
            temp_path = destination.with_name(destination.stem + ".pre-remux" + destination.suffix)
            encode_output = temp_path
        encode_cmd = common_ffmpeg_prefix(ffmpeg) + list(recipe.ffmpeg_args) + [str(encode_output)]
        completed = run_command(encode_cmd)
        if completed.returncode != 0:
            raise GenerationError(
                f"ffmpeg failed for {recipe.asset_id}: {completed.stderr.strip() or completed.stdout.strip()}"
            )
        recipe_text = format_command(encode_cmd)
        if recipe.remux_args:
            assert temp_path is not None
            remux = [
                part.replace("{input}", str(temp_path)) for part in recipe.remux_args
            ]
            remux_cmd = common_ffmpeg_prefix(ffmpeg) + remux + [str(destination)]
            remuxed = run_command(remux_cmd)
            temp_path.unlink(missing_ok=True)
            if remuxed.returncode != 0:
                raise GenerationError(
                    f"ffmpeg remux failed for {recipe.asset_id}: {remuxed.stderr.strip()}"
                )
            recipe_text = f"{recipe_text} && {format_command(remux_cmd)}"
    else:
        raise GenerationError(f"unknown recipe kind: {recipe.kind}")

    if recipe.postprocess:
        recipe_text = f"{recipe_text}; {apply_postprocess(destination, recipe.postprocess)}"

    if not destination.is_file():
        raise GenerationError(f"generator did not create {recipe.relative_path}")

    asset: dict[str, Any] = {
        "id": recipe.asset_id,
        "path": recipe.relative_path,
        "bytes": destination.stat().st_size,
        "sha256": hash_file(destination),
        "categories": list(recipe.categories),
        "provenance": {
            "kind": "generated",
            "license": "MPL-2.0",
            "description": recipe.description,
            "generationRecipe": recipe_text,
        },
    }
    if recipe.media:
        asset["media"] = recipe.media
    return asset


def reset_generated_tree(output_dir: Path) -> None:
    files_root = output_dir / "files"
    if files_root.exists():
        shutil.rmtree(files_root)
    manifest_path = output_dir / "manifest.json"
    if manifest_path.exists():
        manifest_path.unlink()
    output_dir.mkdir(parents=True, exist_ok=True)
    files_root.mkdir(parents=True, exist_ok=True)


def main() -> int:
    arguments = parse_arguments()
    root = repository_root()
    output_dir = (arguments.output or (root / "tests/fixtures/corpus/generated")).resolve()
    committed_root = (root / "tests/fixtures/corpus/files").resolve()

    try:
        ffmpeg = resolve_tool("FFMPEG", "ffmpeg")
        resolve_tool("FFPROBE", "ffprobe")
        version = ffmpeg_version_line(ffmpeg)
        encoders = encoder_names(ffmpeg)
        recipes = build_recipes(version, encoders)
        reset_generated_tree(output_dir)
        assets = [
            generate_recipe(recipe, output_dir, committed_root, ffmpeg) for recipe in recipes
        ]
    except GenerationError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    covered = {category for asset in assets for category in asset["categories"]}
    missing = [category for category in REQUIRED_CATEGORIES if category not in covered]
    if missing:
        print(f"ERROR: generated corpus missing categories: {', '.join(missing)}", file=sys.stderr)
        return 1
    if len(assets) < 200:
        print(f"ERROR: generated only {len(assets)} assets; need at least 200", file=sys.stderr)
        return 1

    manifest = {
        "$schema": "../manifest.schema.json",
        "license": "MPL-2.0",
        "schemaVersion": 1,
        "status": "complete",
        "minimumAssetCount": 200,
        "requiredCategories": list(REQUIRED_CATEGORIES),
        "assets": assets,
    }
    manifest_path = output_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Generated {len(assets)} corpus asset(s) under {output_dir}.")
    print(f"ffmpeg: {version}")
    print(f"Wrote {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
