#!/bin/sh
# Install or update PSPExp on an Onion Omega2+ (OpenWrt 18.06) over SSH:
# the tools (scripts/omega/build.sh), the launcher's fonts, the package
# manager and the console's init script. Everything but the init script and
# a small pspexp-pkg wrapper goes to the SD card (/mnt/mmcblk0p1/pspexp):
# the flash is small. Then the console restarts.
#   scripts/omega/build.sh
#   HOST=root@host scripts/omega/install.sh      (default: the ssh alias "omega"; OMEGA= still read)
# Packages: BOARD=omega HOST=<same host> scripts/packages/pspexp-pkg-build install freedoom
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OM="${HOST:-${OMEGA:-omega}}"
B="$ROOT/build/omega/bin"
DIR=/mnt/mmcblk0p1/pspexp
[ -x "$B/pspexp-term" ] || { echo "run scripts/omega/build.sh first" >&2; exit 1; }
ssh "$OM" "mkdir -p $DIR/assets $DIR/opt/apps $DIR/opt/systems $DIR/home $DIR/.new"
# dropbear: the classic scp protocol (-O)
scp -O -q "$B/pspexp-term" "$B/pspexp-ms" "$B/pspexp-bench" "$B/pspexp-sprite-demo" "$ROOT/scripts/board/pspexp-exec" \
	"$ROOT/scripts/board/pspexp-pkg" "$OM:$DIR/.new/"
scp -O -q "$ROOT/tools/pspexp-term/assets/Inter-Regular.ttf" "$ROOT/tools/pspexp-term/assets/Inter-SemiBold.ttf" \
	"$ROOT/tools/pspexp-term/assets/lucide.ttf" "$ROOT/tools/pspexp-term/assets/Inter-LICENSE.txt" \
	"$ROOT/tools/pspexp-term/assets/lucide-LICENSE.txt" "$OM:$DIR/assets/"
scp -O -q "$ROOT/scripts/omega/pspexp-term.init" "$OM:/tmp/pspexp-term.init"
# pspexp-pkg for the Omega: the packages' files on the SD card
W=$(mktemp); trap 'rm -f "$W"' EXIT
printf '#!/bin/sh\n# PSPExp packages on the Omega: their files on the SD card\nexport PSPEXP_ROOT=%s/opt PATH=%s:$PATH\nexec sh %s/pspexp-pkg "$@"\n' "$DIR" "$DIR" "$DIR" > "$W"
scp -O -q "$W" "$OM:/tmp/pspexp-pkg.wrapper"
# a running program cannot be overwritten: new files are moved into place
ssh "$OM" "set -e
	for f in $DIR/.new/*; do mv -f \"\$f\" $DIR/; done
	rmdir $DIR/.new
	install -m 755 /tmp/pspexp-term.init /etc/init.d/pspexp-term 2>/dev/null || { cp /tmp/pspexp-term.init /etc/init.d/pspexp-term; chmod 755 /etc/init.d/pspexp-term; }
	cp /tmp/pspexp-pkg.wrapper /usr/bin/pspexp-pkg && chmod 755 /usr/bin/pspexp-pkg
	/etc/init.d/pspexp-term enable
	/etc/init.d/pspexp-term restart"
echo "PSPExp installed on $OM ($DIR); the console restarted with the launcher"
