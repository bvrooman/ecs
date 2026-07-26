#!/usr/bin/env bash
# Format every tracked C++ file, or check that they are already formatted.
#
#   tools/format.sh            # reformat in place
#   tools/format.sh --check    # exit non-zero if anything is unformatted (CI)
#   CLANG_FORMAT=... tools/format.sh
#
# .clang-format uses BinPackParameters: OnePerLine, which is an enum only from
# clang-format 20 (it was a bool before). An older binary does not merely
# format differently -- it refuses to read the config at all, with a confusing
# "invalid boolean" error. So the version is checked up front, and note that
# the repo's C++ toolchain is clang-18: the compiler and the formatter are
# deliberately different versions.

set -uo pipefail

cd "$(dirname "$0")/.."

MIN_MAJOR=20

find_clang_format() {
    if [[ -n ${CLANG_FORMAT:-} ]]; then echo "$CLANG_FORMAT"; return; fi
    for c in clang-format-20 clang-format; do
        command -v "$c" > /dev/null 2>&1 && { echo "$c"; return; }
    done
    echo ""
}

CF=$(find_clang_format)
if [[ -z $CF ]]; then
    echo "error: no clang-format found (need >= $MIN_MAJOR)" >&2
    echo "  pip install clang-format==20.1.7" >&2
    exit 2
fi

version=$("$CF" --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
major=${version%%.*}
if [[ -z $major ]] || ((major < MIN_MAJOR)); then
    echo "error: $CF is version ${version:-unknown}; .clang-format needs >= $MIN_MAJOR" >&2
    echo "  (BinPackParameters: OnePerLine is not understood before $MIN_MAJOR)" >&2
    echo "  pip install clang-format==20.1.7" >&2
    exit 2
fi

mapfile -t files < <(git ls-files '*.hpp' '*.cpp')

if [[ ${1:-} == --check ]]; then
    if ! "$CF" --dry-run -Werror "${files[@]}" 2> /tmp/ecs-fmt-err; then
        echo "the following files are not formatted:" >&2
        # keep only "path:line:col: error: ..." diagnostics, not the echoed
        # source lines and caret markers that follow each one
        grep -oE '^[^ :]+:[0-9]+:[0-9]+: error:' /tmp/ecs-fmt-err \
            | cut -d: -f1 | sort -u | sed 's/^/  /' >&2
        echo >&2
        echo "run tools/format.sh to fix" >&2
        exit 1
    fi
    echo "all ${#files[@]} files are formatted ($CF $version)"
else
    "$CF" -i "${files[@]}"
    echo "formatted ${#files[@]} files ($CF $version)"
fi
