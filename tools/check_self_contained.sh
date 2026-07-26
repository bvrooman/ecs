#!/usr/bin/env bash
# Compile every library header on its own, against an otherwise empty
# translation unit. A header that only compiles because some other header was
# included first is a latent break: it works until an include list is
# reordered, a file is split, or a consumer includes it directly.
#
#   tools/check_self_contained.sh [compiler] [std]
#
# The two P2996 backends are skipped: they are reached only under
# #if ECS_USE_P2996 and need a reflection toolchain by construction.

set -uo pipefail

cd "$(dirname "$0")/.."

CXX=${1:-${CXX:-clang++}}
STD=${2:-c++23}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

SKIP=(
    ecs/detail/for_each_row_p2996.hpp
    ecs/reflection/reflect_p2996.hpp
)

failed=0
checked=0
for header in $(cd include && find ecs -name '*.hpp' | sort); do
    for s in "${SKIP[@]}"; do
        [[ $header == "$s" ]] && continue 2
    done
    echo "#include <$header>" > "$TMP/tu.cpp"
    if ! "$CXX" -std="$STD" -fsyntax-only -Iinclude -Itools/include "$TMP/tu.cpp" 2> "$TMP/err"; then
        echo "not self-contained: $header"
        sed 's/^/    /' "$TMP/err" | head -5
        failed=$((failed + 1))
    fi
    checked=$((checked + 1))
done

for header in $(cd tools/include && find ecs -name '*.hpp' | sort); do
    echo "#include <$header>" > "$TMP/tu.cpp"
    if ! "$CXX" -std="$STD" -fsyntax-only -Iinclude -Itools/include "$TMP/tu.cpp" 2> "$TMP/err"; then
        echo "not self-contained: tools/include/$header"
        sed 's/^/    /' "$TMP/err" | head -5
        failed=$((failed + 1))
    fi
    checked=$((checked + 1))
done

if ((failed)); then
    echo "$failed of $checked headers are not self-contained"
    exit 1
fi
echo "all $checked headers are self-contained"
