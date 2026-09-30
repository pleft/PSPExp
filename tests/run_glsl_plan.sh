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
# a reflection: lit, a cube map added (a pass of its own); glass: the cube map only, a discard on
# another texture (a pass before, for the depth)
R="$ROOT/tests/glsl/reflect.vert" P="uMVP=1,0,0,0,0,1,0,0,0,0,-1.2222,-1,0,0,-2.2222,0"
OUT="$("$BUILD/glsl_plan" "$R" "$ROOT/tests/glsl/reflect.frag" uSky=2 uLight=0,0,1,0 uDiffuse=0.8,0.8,0.8,1 uAmbient=0.2,0.2,0.2,1 "$P")"
check "plan: exact"; check "lit: normal attribute 1"; check "cube map: unit 2, added, normal attribute 1"; check "facing: 0.00 0.00 1.00"
OUT="$("$BUILD/glsl_plan" "$R" "$ROOT/tests/glsl/glass.frag" uSky=2 "$P" uEnvRot=0,0,-1,0,0,1,0,0,1,0,0,0,0,0,0,1)"
check "plan: exact"; check "pre-pass: unit 0, alpha test 0x201 0.500"; check "cube map: unit 2, the colour, its alpha"; check "facing: 1.00 0.00 -0.00"
OUT="$("$BUILD/glsl_plan" "$R" "$ROOT/tests/glsl/glass.frag" uSky=2 "$P" uPattern=1)"
check "nearest: a pattern on the pixels' places"; check "pre-pass: unit 0, alpha test 0x201 0.500"
# a lightmapped wall: the lightmap multiplied in (twice over, 8 times: a brighter texture), a glow added
L="$ROOT/tests/glsl/lightmap.vert" LF="$ROOT/tests/glsl/lightmap.frag"
OUT="$("$BUILD/glsl_plan" "$L" "$LF" uWall=0 uLight=1 uGlow=2 uBright=2 uUseGlow=1)"
check "plan: exact"; check "texture: yes unit 0"; check "multiplied in: unit 1, 2 times over"; check "added: unit 2"; check "alpha test: 0x206 0.500"
OUT="$("$BUILD/glsl_plan" "$L" "$LF" uWall=0 uLight=1 uGlow=2 uBright=8 uUseGlow=0)"
check "plan: exact"; check "multiplied in: unit 1, 8 times over"
if echo "$OUT" | grep -q "added:"; then echo "$OUT"; echo "no added texture expected"; exit 1; fi
# returns under a condition in a function: each side where it holds (the test vertex's normal: 0.1, 0.2, 0.3)
SV="$ROOT/tests/glsl/shade.vert" SF="$ROOT/tests/glsl/shade.frag"
OUT="$("$BUILD/glsl_plan" "$SV" "$SF" uLightDir=0,0,1)"
check "plan: exact"; check "rgba 0.65 0.65 0.65 1.00"
OUT="$("$BUILD/glsl_plan" "$SV" "$SF" uLightDir=0,0,-1)"
check "plan: exact"; check "rgba 0.46 0.46 0.46 1.00"
echo "glsl_plan: ok"
