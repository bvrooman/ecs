#!/usr/bin/env bash
# Format every tracked C++ file, or check that they are already formatted.
#
#   tools/format.sh            # reformat in place
#   tools/format.sh --check    # exit non-zero if anything is unformatted (CI)
#   CLANG_FORMAT=... tools/format.sh
#
# The version is pinned in .clang-format-version, which CI installs and this
# script enforces -- one source of truth, so a local run and CI cannot disagree.
#
# Pinning matters twice over:
#
#   * Too old and the config does not load at all. .clang-format uses
#     BinPackParameters: OnePerLine, an enum only from clang-format 20; an
#     older binary fails with a confusing "invalid boolean" rather than
#     formatting differently. Note the repo's C++ toolchain is clang-18 -- the
#     compiler and the formatter are deliberately different versions.
#
#   * A different major formats differently. clang-format 20 and 22 disagree on
#     AlignConsecutiveAssignments when a right-hand side wraps, for instance, so
#     an unpinned check flaps as contributors and runners drift apart.

set -uo pipefail

cd "$(dirname "$0")/.."

PIN=$(< .clang-format-version)
PIN_MAJOR=${PIN%%.*}

find_clang_format() {
    if [[ -n ${CLANG_FORMAT:-} ]]; then echo "$CLANG_FORMAT"; return; fi
    for c in "clang-format-$PIN_MAJOR" clang-format; do
        command -v "$c" > /dev/null 2>&1 && { echo "$c"; return; }
    done
    echo ""
}

CF=$(find_clang_format)
if [[ -z $CF ]]; then
    echo "error: no clang-format found (this tree is formatted with $PIN)" >&2
    echo "  pip install clang-format==$PIN" >&2
    exit 2
fi

version=$("$CF" --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
major=${version%%.*}

if [[ -z $major ]] || ((major != PIN_MAJOR)); then
    echo "error: $CF is ${version:-unknown}; this tree is formatted with $PIN" >&2
    if [[ -n $major ]] && ((major < PIN_MAJOR)); then
        echo "  older majors format differently, and before 20 the config does not load at all" >&2
    else
        echo "  newer majors format differently -- CI would reject the result" >&2
    fi
    echo "  pip install clang-format==$PIN" >&2
    exit 2
fi

# Same major, different patch: output is very unlikely to differ, so this is a
# warning rather than an error -- but say so, since it is the first thing to
# suspect if CI disagrees with a local run.
if [[ $version != "$PIN" ]]; then
    echo "warning: $CF is $version, pinned is $PIN (same major; output should match)" >&2
fi

# --cached --others --exclude-standard: tracked files PLUS new ones that are
# not yet committed, minus anything gitignored (build dirs carry generated
# .cpp files). Plain `git ls-files` lists only tracked files, so a new file
# would pass this check locally and then fail in CI once committed.
mapfile -t files < <(git ls-files --cached --others --exclude-standard '*.hpp' '*.cpp' | sort -u)

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
