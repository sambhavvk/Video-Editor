#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

usage() {
  cat <<'EOF'
Usage: format_changed_cpp.sh [--check|--apply] [--base GIT_REF]

Checks or formats changed C/C++ and Objective-C++ source files. Untracked files
are included. With no --base, the script compares with HEAD and also includes
staged and unstaged changes.
EOF
}

mode=check
base_ref=
while (($# > 0)); do
  case "$1" in
    --check)
      mode=check
      shift
      ;;
    --apply)
      mode=apply
      shift
      ;;
    --base)
      if (($# < 2)); then
        echo "--base requires a Git ref" >&2
        exit 64
      fi
      base_ref=$2
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

if ! repository_root=$(git rev-parse --show-toplevel 2>/dev/null); then
  echo "This script must run inside a Git worktree." >&2
  exit 2
fi
cd -- "${repository_root}"

formatter=${CLANG_FORMAT:-clang-format}
if ! command -v "${formatter}" >/dev/null 2>&1; then
  echo "clang-format executable not found: ${formatter}" >&2
  exit 2
fi

changed_list=$(mktemp)
trap 'rm -f -- "${changed_list}"' EXIT

if git rev-parse --verify HEAD >/dev/null 2>&1; then
  if [[ -n ${base_ref} ]]; then
    if ! git rev-parse --verify "${base_ref}^{commit}" >/dev/null 2>&1; then
      echo "Unknown base ref: ${base_ref}" >&2
      exit 2
    fi
    git diff --name-only --diff-filter=ACMR -z "${base_ref}" -- > "${changed_list}"
  else
    {
      git diff --name-only --diff-filter=ACMR -z HEAD --
      git diff --cached --name-only --diff-filter=ACMR -z HEAD --
    } > "${changed_list}"
  fi
else
  : > "${changed_list}"
fi
git ls-files --others --exclude-standard -z >> "${changed_list}"

declare -a source_files=()
while IFS= read -r -d '' path; do
  case "${path}" in
    *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.hxx|*.m|*.mm)
      [[ -f ${path} ]] && source_files+=("${path}")
      ;;
  esac
done < <(sort -zu "${changed_list}")

if ((${#source_files[@]} == 0)); then
  echo "No changed C/C++ files to format."
  exit 0
fi

if [[ ${mode} == apply ]]; then
  "${formatter}" -i --style=file "${source_files[@]}"
  echo "Formatted ${#source_files[@]} changed file(s)."
else
  "${formatter}" --dry-run --Werror --style=file "${source_files[@]}"
  echo "Formatting check passed for ${#source_files[@]} changed file(s)."
fi

