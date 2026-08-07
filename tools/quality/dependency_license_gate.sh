#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

usage() {
  cat <<'EOF'
Usage: dependency_license_gate.sh [--source-only] [--official --audit PATH]

Source mode validates repository licensing markers, pinned dependency contracts,
and release-source metadata. Official mode additionally requires every Flatpak
source lock to be resolved and runs the compiled FFmpeg audit with --official.
EOF
}

mode=source
audit_binary=

while (($# > 0)); do
  case "$1" in
    --source-only)
      mode=source
      shift
      ;;
    --official)
      mode=official
      shift
      ;;
    --audit)
      if (($# < 2)); then
        echo "--audit requires a path" >&2
        exit 64
      fi
      audit_binary=$2
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 64
      ;;
  esac
done

script_directory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repository_root=$(cd -- "${script_directory}/../.." && pwd)
cd -- "${repository_root}"

failures=0
fail() {
  echo "ERROR: $*" >&2
  failures=$((failures + 1))
}

if [[ ! -f LICENSE ]] || ! grep -q 'Mozilla Public License Version 2.0' LICENSE; then
  fail "LICENSE is missing or is not the MPL 2.0 text"
fi
if [[ ! -f THIRD_PARTY.md ]]; then
  fail "THIRD_PARTY.md is required"
fi

dependency_file=cmake/DependencyVersions.cmake
if [[ ! -f ${dependency_file} ]]; then
  fail "${dependency_file} is missing"
else
  required_version_variables=(
    VIDEO_EDITOR_QT_VERSION
    VIDEO_EDITOR_FFMPEG_VERSION
    VIDEO_EDITOR_AVFORMAT_VERSION
    VIDEO_EDITOR_AVCODEC_VERSION
    VIDEO_EDITOR_AVUTIL_VERSION
    VIDEO_EDITOR_LIBPLACEBO_VERSION
    VIDEO_EDITOR_ABSEIL_VERSION
    VIDEO_EDITOR_PROTOBUF_VERSION
    VIDEO_EDITOR_EBUR128_VERSION
  )
  for variable in "${required_version_variables[@]}"; do
    declaration=$(grep -E "^set\(${variable} \"[0-9]+(\.[0-9]+)+\"\)$" "${dependency_file}" || true)
    if [[ -z ${declaration} ]]; then
      fail "${variable} must be an exact numeric version in ${dependency_file}"
    fi
  done
fi

for source_file in \
  .github/workflows/*.yml \
  packaging/flatpak/*.yml \
  packaging/flatpak/*.desktop \
  packaging/flatpak/*.xml \
  packaging/flatpak/*.svg \
  packaging/windows/*.wxs \
  packaging/windows/*.ps1 \
  tools/quality/*.sh \
  tools/quality/*.py; do
  [[ -e ${source_file} ]] || continue
  if ! head -n 5 "${source_file}" | grep -q 'SPDX-License-Identifier:'; then
    fail "missing SPDX marker near the start of ${source_file}"
  fi
done

if grep -R -E --line-number \
  --include='*.cmake' --include='CMakeLists.txt' --include='*.yml' \
  -- '--enable-(gpl|nonfree)|FFMPEG.*(GPL|NONFREE).*ON' cmake packaging/flatpak; then
  fail "distribution configuration enables an FFmpeg GPL or nonfree option"
fi

release_lock=packaging/flatpak/release-sources.json
if [[ ! -f ${release_lock} ]]; then
  fail "${release_lock} is missing"
else
  if ! python3 - "${release_lock}" "${mode}" <<'PY'
import json
import re
import sys
from pathlib import Path

path = Path(sys.argv[1])
mode = sys.argv[2]
try:
    data = json.loads(path.read_text(encoding="utf-8"))
except (OSError, json.JSONDecodeError) as exc:
    print(f"ERROR: cannot read {path}: {exc}", file=sys.stderr)
    raise SystemExit(1)

sources = data.get("sources")
if (
    data.get("schemaVersion") != 1
    or data.get("license") != "MPL-2.0"
    or not isinstance(sources, list)
    or not sources
):
    print(f"ERROR: malformed release source lock: {path}", file=sys.stderr)
    raise SystemExit(1)

blocked = []
for source in sources:
    if not isinstance(source, dict) or not isinstance(source.get("name"), str):
        print(f"ERROR: malformed source entry in {path}", file=sys.stderr)
        raise SystemExit(1)
    status = source.get("status")
    url = source.get("archiveUrl")
    digest = source.get("sha256")
    pinned = (
        status == "pinned"
        and isinstance(url, str)
        and url.startswith("https://")
        and isinstance(digest, str)
        and re.fullmatch(r"[0-9a-f]{64}", digest) is not None
    )
    if not pinned:
        blocked.append(source["name"])

declared_blocking = data.get("releaseBlocking") is True
if mode == "official" and (declared_blocking or blocked):
    print(
        "ERROR: unresolved release sources: " + ", ".join(blocked or ["releaseBlocking"]),
        file=sys.stderr,
    )
    raise SystemExit(1)
if blocked:
    print("Release source blockers (expected during development): " + ", ".join(blocked))
PY
  then
    fail "Flatpak release source lock did not pass"
  fi
fi

if [[ ${mode} == official ]]; then
  if [[ -z ${audit_binary} ]]; then
    fail "official mode requires --audit PATH"
  elif [[ ! -x ${audit_binary} ]]; then
    fail "FFmpeg dependency audit is not executable: ${audit_binary}"
  elif ! "${audit_binary}" --official; then
    fail "compiled FFmpeg runtime failed the official LGPL/ABI audit"
  fi
fi

if ((failures > 0)); then
  echo "Dependency/license gate failed with ${failures} error(s)." >&2
  exit 1
fi

echo "Dependency/license gate passed (${mode} mode)."
