#!/bin/sh
# make-sdl12-rename.sh SDL_PSPEXP_C OUT_H: for the layer's build for SDL 1.2 programs (PSPEXP_SDL12_MODE),
# its hooks renamed pspexp_SDL_* (its own calls to them included), each declared with SDL2's prototype:
# made from the file's HOOK( lines
set -e
{
	echo "/* made by make-sdl12-rename.sh from sdl_pspexp.c: do not edit */"
	sed -n 's/^HOOK([^,]*, *\(SDL_[A-Za-z0-9_]*\),.*/\1/p' "$1" | sort -u | while read -r f; do
		echo "__typeof__($f) pspexp_$f;"
		echo "#define $f pspexp_$f"
	done
} > "$2.tmp"
mv "$2.tmp" "$2"
