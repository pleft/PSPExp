# Helpers for package recipes (sourced). $CACHE keeps downloads between builds.
# fetch URL [NAME]   -> prints the cached file's path (downloads it once)
fetch() {
	f="$CACHE/${2:-$(basename "$1")}"
	if [ ! -s "$f" ]; then
		curl -fsSL --retry 2 -o "$f.part" "$1" || { rm -f "$f.part"; echo "fetch failed: $1" >&2; return 1; }
		mv "$f.part" "$f"
	fi
	echo "$f"
}
# the Doom key map for the PSP (Chocolate Doom's defaults)
DOOM_KEYS="cross=lctrl,circle=space,square=lshift,triangle=return+y,l=comma,r=period,start=escape,select=tab,up=up,down=down,left=left,right=right"

# ---- emulated systems: pspexp-retro + one libretro core ----------------------
# retro_core DIR CORE        puts the player and CORE into the package DIR for
#                            $BOARD (building them when missing) and sets
#                            RETRO_EXEC (the system's exec) and RETRO_REQ
#   gameshell      pspexp-retro + CORE_libretro.so, cross-compiled on the Mac
#                  (scripts/gameshell/build.sh retro)
#   pi             the same, compiled on the Pi (scripts/pi/install.sh retro)
#   omega          pspexp-retro-CORE, one static program (build-omega.sh retro)
# Core sources: ../retro/libretro-CORE next to the repository.
retro_core() {
	_d="$1"; _c="$2"
	case "$BOARD" in
	gameshell|pi)
		_b="$ROOT/build/$BOARD/bin"
		if [ ! -x "$_b/pspexp-retro" ] || [ ! -f "$_b/${_c}_libretro.so" ]; then
			if [ "$BOARD" = pi ]; then CORE=$_c HOST="$HOST" "$ROOT/scripts/pi/install.sh" retro
			else CORE=$_c "$ROOT/scripts/gameshell/build.sh" retro; fi   # cross-compiled on the Mac
		fi
		cp "$_b/pspexp-retro" "$_b/${_c}_libretro.so" "$_d/"
		RETRO_EXEC="pspexp-run ./pspexp-retro ./${_c}_libretro.so %ROM%"
		RETRO_REQ="pspexp-run" ;;
	omega)
		_bin="$ROOT/build/omega/bin/pspexp-retro-$_c"
		[ -x "$_bin" ] || CORE=$_c "$ROOT/scripts/omega/build.sh" retro
		cp "$_bin" "$_d/pspexp-retro-$_c"
		RETRO_EXEC="pspexp-exec ./pspexp-retro-$_c $_c %ROM%"
		RETRO_REQ="pspexp-exec" ;;
	*) echo "$_c: no build for $BOARD" >&2; exit 3 ;;
	esac
	chmod +x "$_d"/pspexp-retro*
	for _l in COPYING Copying LICENSE LICENSE.txt license.txt copyright; do
		[ -f "$ROOT/../retro/libretro-$_c/$_l" ] && { cp "$ROOT/../retro/libretro-$_c/$_l" "$_d/COPYING.$_c"; break; }
	done
	RETRO_VERSION=$(git -C "$ROOT/../retro/libretro-$_c" rev-parse --short HEAD 2>/dev/null || echo unknown)
}

# the PSP's buttons as the player's keyboard keys too: not every SDL has
# virtual joysticks (the Omega's static one has none), keys always work
RETRO_KEYS="cross=z,circle=x,square=a,triangle=s,l=q,r=w,start=return,select=rshift,up=up,down=down,left=left,right=right"

# retro_system DIR ID NAME SHORT EXT THUMBS   writes DIR/systems/ID.system
# (the package directory is DIR's name; ROMs in ~/roms/ID)
retro_system() {
	mkdir -p "$1/systems"
	cat > "$1/systems/$2.system" <<S
name=$3
short=$4
icon=gamepad-2
roms=~/roms/$2
ext=$5
exec=$RETRO_EXEC
pkg=$(basename "$1")
env=SDL_VIDEODRIVER=dummy
env=SDL_AUDIODRIVER=dummy
env=PSPEXP_KEYS=$RETRO_KEYS
env=PSPEXP_NUB=keys
thumbs=$6
pspexp=1
S
	# the Omega2+ cannot move 60 whole NES-sized frames a second to the PSP
	# (texture work and USB cost its CPU ~20 ms a frame when a game scrolls):
	# a steady 30 fps picture, the game and its sound at full speed. Recipes
	# with small screens set OMEGA_VIDEO_DIV=1 (the Game Boy's 160x144 keeps up)
	[ "$BOARD" = omega ] && [ "${OMEGA_VIDEO_DIV:-2}" -gt 1 ] && echo "env=PSPEXP_RETRO_VIDEO_DIV=${OMEGA_VIDEO_DIV:-2}" >> "$1/systems/$2.system"
	return 0
}
