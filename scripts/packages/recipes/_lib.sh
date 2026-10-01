# Helpers for the Omega2+'s package build scripts (sourced; the Debian boards'
# packages are the recipes in recipes/, through apt). $CACHE keeps downloads between builds.
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
#   omega          pspexp-retro-CORE, one static program (build-omega.sh retro)
# Core sources: ../retro/libretro-CORE next to the repository.
retro_core() {
	_d="$1"; _c="$2"
	case "$BOARD" in
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
# the changes to NXEngine-evo's source, as a shell command run in the source
# tree: the first start at 480x272 (the PSP's screen, its resolution 6)
# instead of 640x480; and dying before the first save starts over, as the
# original does (NXEngine-evo stops with "savefile error" instead)
cavestory_patch() {
	cat <<'PATCH'
sed -i.orig 's|setfile->resolution     = 2; // 640x480 Windowed, should be safe value|setfile->resolution     = 6; // 480x272, the PSP screen (PSPExp)|' src/settings.cpp && grep -q 'resolution     = 6;' src/settings.cpp &&
perl -0pi -e 's/(if \(game_load\(settings->last_save_slot\)\)\n(\s*)\{\n)/$1$2  if (game.switchstage.mapno == LOAD_GAME) { game.switchstage.mapno = NEW_GAME; continue; } \/\/ no save yet: start over (PSPExp)\n/' src/main.cpp && grep -q 'start over (PSPExp)' src/main.cpp
PATCH
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

# ---- Jazz Jackrabbit (OpenJazz) ----------------------------------------------
# the shareware release (v1.1, 78 files, 1994) from the DOS Games Archive,
# checked against its SHA-256; prints the cached zip's path
jazz_zip() {
	_z="$CACHE/jazz.zip"
	if [ ! -s "$_z" ]; then
		curl -fsSL -A 'Mozilla/5.0' -e https://www.dosgamesarchive.com/file/jazz-jackrabbit/jazz \
			-o "$_z.part" "https://www.dosgamesarchive.com/file.php?id=2031" || { rm -f "$_z.part"; echo "fetch failed: jazz.zip" >&2; return 1; }
		mv "$_z.part" "$_z"
	fi
	_h=$(shasum -a 256 "$_z" 2>/dev/null || sha256sum "$_z")
	case "$_h" in
	ed025415c0bc5ebc3a41e7a070551bdfdfb0b65b5314241152d8bd31f87c22da*) echo "$_z" ;;
	*) echo "jazz.zip: unexpected SHA-256" >&2; return 1 ;;
	esac
}
# the PSP's buttons as OpenJazz's keys: cross jumps (Enter and Y in menus and
# questions), square fires, circle backs out (N), triangle, L and R change
# weapon, start the menu, select pauses
JAZZ_KEYS="cross=space+return+y,square=lalt,circle=escape+n,triangle=rctrl,l=rctrl,r=rctrl,start=escape,select=p,up=up,down=down,left=left,right=right"

# check_sha256 FILE HASH   stops the recipe when a download is not the one pinned
check_sha256() {
	_h=$(shasum -a 256 "$1" 2>/dev/null || sha256sum "$1")
	case "$_h" in "$2"*) return 0 ;; esac
	echo "$(basename "$1"): unexpected SHA-256" >&2; exit 1
}

# ---- Simutrans ---------------------------------------------------------------
# the nub is the mouse (cross and circle its buttons); the D-pad scrolls the
# map, L/R zoom (Page Down/Up), triangle and start close the top window
# (Escape), select pauses
SIMUTRANS_KEYS="up=up,down=down,left=left,right=right,l=pgdn,r=pgup,triangle=escape,start=escape,select=p,square=none"

# ---- ScummVM's freeware adventures ------------------------------------------
# scummvm_game DIR ID ENGINE SUBDIR DEB...   ScummVM (scripts/omega/build.sh
# scummvm, built when missing) and the game's data into the package DIR: the
# data from Debian's own packages (architecture-independent, the same files
# the Debian boards install), unpacked to DIR/data with links resolved (the
# Omega's card is FAT; only SUBDIR, the language played), ScummVM's engine
# data file beside it. Sets
# SCUMMVM_EXEC (the entry's exec: the game in DIR/data/SUBDIR). DEB is a path
# in Debian's pool, e.g. b/beneath-a-steel-sky/beneath-a-steel-sky_0.0372-9_all.deb
scummvm_game() {
	_d=$1 _id=$2 _eng=$3 _sub=$4; shift 4
	case "${BOARD:-omega}" in
	omega) _bin="$ROOT/build/omega/bin/scummvm"
	       [ -x "$_bin" ] || "$ROOT/scripts/omega/build.sh" scummvm ;;
	*) echo "$_id: no build for $BOARD" >&2; exit 3 ;;
	esac
	_w="$_d/.unpack"
	rm -rf "$_w"; mkdir -p "$_w/root" "$_d/data"
	for _deb in "$@"; do
		_f=$(fetch "https://deb.debian.org/debian/pool/main/$_deb") || exit 1
		(cd "$_w" && rm -f data.tar.* && ar -x "$_f" && tar -xf data.tar.* -C root)
	done
	# only the folder played (Drascula and Lure have one per language, Debian
	# linking the shared files and music into each: resolved, five copies)
	mkdir -p "$_d/data/$_sub"
	cp -RL "$_w/root/usr/share/scummvm/$_id/$_sub/." "$_d/data/$_sub/"
	rm -rf "$_w"
	_e="$ROOT/../scummvm/scummvm-2.9.0/dists/engine-data"
	for _x in "$_e/$_eng.dat" "$_e/$_eng.tbl" "$_e/$_eng.cpt"; do [ -f "$_x" ] && cp "$_x" "$_d/data/$_sub/"; done
	cp "$_bin" "$_d/scummvm"
	chmod +x "$_d/scummvm"
	cp "$ROOT/../scummvm/scummvm-2.9.0/COPYING" "$_d/COPYING.scummvm"
	cp "$ROOT/recipes/$_id/cover.png" "$_d/$_id.png"   # the entry's cover (the Debian boards' too)
	SCUMMVM_EXEC="env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy pspexp-exec ./scummvm --gfx-mode=surfacesdl --scaler=normal --scale-factor=1 --no-fullscreen -p ./data/$_sub $_eng"
}
# as the Debian boards' recipes: the nub the mouse (cross and circle its
# buttons), start ScummVM's menu (F5), select skips (Escape), triangle skips a
# line, square pauses
SCUMMVM_KEYS="start=f5,select=escape,triangle=period,square=space,up=up,down=down,left=left,right=right"
