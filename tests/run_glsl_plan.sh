#!/bin/sh
# CTest: the GLSL reader's plans for a shader pair, with uniforms set two ways.
#   run_glsl_plan.sh <build dir>
set -e
BUILD="$1"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
V="$ROOT/tests/glsl/texcolor.vert" F="$ROOT/tests/glsl/texcolor.frag"
check() { echo "$OUT" | grep -q "$1" || { echo "$OUT"; echo "expected: $1"; exit 1; }; }
OUT="$("$BUILD/glsl_plan" "$V" "$F" uDiscard=1)"
check "plan: exact"; check "matrix x attribute 0"; check "texture: yes unit 0, alpha yes"; check "alpha test: 0x206 0.500"
check "rgba 0.10 0.20 0.30 0.50"
OUT="$("$BUILD/glsl_plan" "$V" "$F" uDiscard=0 uWhite=1)"
check "plan: exact"
if echo "$OUT" | grep -q "alpha test"; then echo "$OUT"; echo "no alpha test expected"; exit 1; fi
check "rgba 1.00 1.00 1.00 1.00"
echo "glsl_plan: ok"
