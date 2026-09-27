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
	# a recipe may draw only every Nth frame on the Omega2+ (OMEGA_VIDEO_DIV=2:
	# a steady 30 fps picture, the game and its sound at full speed) for a
	# system it cannot keep up with; none needs it since the SDL layer of
	# 2026-09-27 (the NES plays at 60: 47 fps shown on average, 38 at worst)
	[ "$BOARD" = omega ] && [ "${OMEGA_VIDEO_DIV:-1}" -gt 1 ] && echo "env=PSPEXP_RETRO_VIDEO_DIV=${OMEGA_VIDEO_DIV}" >> "$1/systems/$2.system"
	return 0
}

# ---- Cave Story (NXEngine-evo) ----------------------------------------------
# the freeware Cave Story (2004, English by Aeon Genesis), checked against the
# SHA-512 NXEngine-evo's Flatpak pins; prints the cached zip's path
cavestory_zip() {
	_z=$(fetch "https://www.cavestory.org/downloads/cavestoryen.zip") || return 1
	_h=$(shasum -a 512 "$_z" 2>/dev/null || sha512sum "$_z")
	case "$_h" in
	9c7c2c6c8114974df14b1dce379b86da48caf299716e1de3d743e47c3ac6a8c13c4a0ada85778e69f11c2428f991dcbfcb0020212279f9c1787260102988cbc3*) echo "$_z" ;;
	*) echo "cavestoryen.zip: unexpected SHA-512" >&2; return 1 ;;
	esac
}
# the one change to NXEngine-evo's source: the first start at 480x272 (the
# PSP's screen, its resolution 6) instead of 640x480; a shell command, run in
# the source tree
cavestory_patch() {
	echo "sed -i.orig 's|setfile->resolution     = 2; // 640x480 Windowed, should be safe value|setfile->resolution     = 6; // 480x272, the PSP screen (PSPExp)|' src/settings.cpp && grep -q 'resolution     = 6;' src/settings.cpp;"
}
# the PSP's buttons as Cave Story's keys: cross jumps (and accepts), square
# and circle fire (circle also declines), L/R change weapon, triangle the
# inventory, select the map, start the menu
CAVESTORY_KEYS="cross=z,square=x,circle=x,l=a,r=s,triangle=q,select=w,start=escape,up=up,down=down,left=left,right=right"
# the game data, the same on every board: NXEngine-evo's nxextract (built here
# with the Mac's compiler; it needs only SDL's headers) turns the freeware
# Doukutsu.exe into data files, and NXEngine-evo's own data goes over them;
# prints the directory (build/cavestory-data)
cavestory_data() {
	_o="$ROOT/build/cavestory-data"
	[ -f "$_o/Stage/Cave.pxm" ] && [ -f "$_o/sprites.sif" ] && { echo "$_o"; return 0; }
	_s="$ROOT/../cavestory/nxengine-evo" _w="$ROOT/build/cavestory"
	_inc="$ROOT/build/gameshell/gnu/src/SDL2-2.30.12/include"
	[ -f "$_inc/SDL.h" ] || _inc="$ROOT/build/omega/prefix/include/SDL2"
	mkdir -p "$_w"
	if [ ! -x "$_w/nxextract" ]; then
		c++ -w -std=c++14 -O2 -DHAVE_UNIX_LIKE -I"$_s" -I"$_s/deps" -I"$_inc" "$_s"/src/extract/*.cpp "$_s/src/common/misc.cpp" \
			"$_s/src/Utils/Logger.cpp" "$_s/src/stagedata.cpp" -o "$_w/nxextract" >&2 || return 1
	fi
	_z=$(cavestory_zip) || return 1
	rm -rf "$_w/game" "$_o" && mkdir -p "$_w/game" "$_o"
	unzip -q "$_z" -d "$_w/game"
	(cd "$_w/game/CaveStory" && "$_w/nxextract" >/dev/null) || return 1
	cp -R "$_w/game/CaveStory/data/." "$_o/" && cp -R "$_s/data/." "$_o/"
	echo "$_o"
}
