#!/bin/sh
# The SDL layer's build for SDL 1.2 programs (libsdl-pspexp-sdl12.so) reaches SDL2 only through its
# own lookups (REAL()): it is not linked against SDL2, and in an SDL 1.2 program no SDL2 function is
# there to bind to. An SDL_ symbol it needs from outside is a direct call that fails: at load where
# the library binds everything at once (the GameShell's build: Frozen Bubble ended at its start,
# "undefined symbol: SDL_PeepEvents", 2026-11-01), at the call elsewhere. Fails the build on one.
#   check-sdl12-imports.sh LIB
LIB="$1"
[ -f "$LIB" ] || { echo "check-sdl12-imports: $LIB missing" >&2; exit 1; }
U=$(nm -D --undefined-only "$LIB" 2>/dev/null | awk '{print $NF}' | grep '^SDL_' | sort -u | tr '\n' ' ')
[ -z "$U" ] || { echo "$LIB calls SDL directly (reach it through REAL()): $U" >&2; exit 1; }
