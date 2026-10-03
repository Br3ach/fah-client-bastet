#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CXX=${CXX:-g++}
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT HUP INT TERM
# Isolate the two actual allocator files so quoted includes resolve to stubs.
cp "$ROOT/src/fah/client/CPUResources.cpp" "$BUILD/"
cp "$ROOT/src/fah/client/CPUResources.h" "$BUILD/"
"$CXX" -std=c++17 -Wall -Wextra -Werror -pedantic -fsyntax-only \
  -I"$HERE/stub" "$BUILD/CPUResources.cpp"
"$CXX" -std=c++17 -Wall -Wextra -pedantic \
  -I"$HERE/stub" -I"$BUILD" "$BUILD/CPUResources.cpp" "$HERE/test.cpp" \
  -o "$BUILD/allocator-test"
"$BUILD/allocator-test"
