#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CXX=${CXX:-g++}
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT HUP INT TERM
# Isolate production allocator files so quoted includes resolve to stubs.
cp "$ROOT/src/fah/client/CPUResources.cpp" "$BUILD/"
cp "$ROOT/src/fah/client/CPUResources.h" "$BUILD/"
cp "$ROOT/src/fah/client/CPUExecutionPlan.h" "$BUILD/"
cp "$ROOT/src/fah/client/CPUExecutionPlan.cpp" "$BUILD/"
"$CXX" -std=c++17 -Wall -Wextra -Werror -pedantic -fsyntax-only \
  -I"$HERE/stub" "$BUILD/CPUResources.cpp"
"$CXX" -std=c++17 -Wall -Wextra -pedantic \
  -I"$HERE/stub" -I"$BUILD" "$BUILD/CPUResources.cpp" "$BUILD/CPUExecutionPlan.cpp" "$HERE/test.cpp" \
  -o "$BUILD/allocator-test"
"$BUILD/allocator-test"

"$CXX" -std=c++17 -Wall -Wextra -pedantic \
  -I"$HERE/stub" -I"$BUILD" "$BUILD/CPUResources.cpp" "$BUILD/CPUExecutionPlan.cpp" "$HERE/gpu-reservation.cpp" \
  -o "$BUILD/gpu-reservation-test"
"$BUILD/gpu-reservation-test"

"$CXX" -std=c++17 -Wall -Wextra -pedantic \
  -I"$HERE/stub" -I"$BUILD" "$BUILD/CPUResources.cpp" "$BUILD/CPUExecutionPlan.cpp" "$HERE/process-policy.cpp" \
  -o "$BUILD/process-policy-test"
"$BUILD/process-policy-test"
