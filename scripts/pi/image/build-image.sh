#!/bin/sh
# The PSPExp Raspberry Pi card image: Raspberry Pi OS Lite (64-bit) as Raspberry
# Pi publishes it (its firmware, kernel and first-boot resize untouched), with
# PSPExp from its apt repository (pspexp-core) and the Pi's own files
# (scripts/pi/board-files.sh), ready on first boot: write it to a card, plug the
# PSP into the Pi's USB, power on. No games: the user adds them in Apps > Packages.
# For Raspberry Pi 3, 4, 400, 5 and 500 (64-bit). Built on this machine in a
# container (OrbStack/Docker):
#   scripts/pi/image/build-image.sh [options]
#       -> build/pi-image/pspexp-pi.img  (write it to a card)
# Options (for your own card; they never go into the repository):
#   --ssh-key FILE   allow this public key for pi and switch SSH on (default: SSH off)
# Login: pi / pspexp (change it), sudo without a password as Raspberry Pi OS sets
# up its first user. The Raspberry Pi Imager's settings (Wi-Fi, SSH, a user)
# still apply on top: they come through cloud-init at first boot.
# The package repository's address: $PSPEXP_REPO_URL, ~/.pspexp/repo-url (a local
# setting), else the public one (scripts/packages/public-repo-url).
set -e
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="$ROOT/build/pi-image"
SSHKEY=""
while [ $# -gt 0 ]; do
	case "$1" in
	--ssh-key) SSHKEY="$2"; shift 2 ;;
	*) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
	esac
done
# the base: one Raspberry Pi OS Lite release, pinned and checked (moving to a newer
# one is a change here, tried before it is committed)
BASE_URL=https://downloads.raspberrypi.com/raspios_lite_arm64/images/raspios_lite_arm64-2026-10-06/2026-10-06-raspios-trixie-arm64-lite.img.xz
BASE_SHA=483db18a48da399b5b7022ffae9a07bc6d99daab30e593aedac09b69ff422843
BASE="$OUT/cache/base.img.xz"; mkdir -p "$OUT/cache"
if [ ! -s "$BASE" ] || [ "$(shasum -a 256 "$BASE" | cut -d' ' -f1)" != "$BASE_SHA" ]; then
	echo "fetching $BASE_URL"
	curl -fL -o "$BASE" "$BASE_URL" || { echo "cannot fetch $BASE_URL" >&2; exit 1; }
	[ "$(shasum -a 256 "$BASE" | cut -d' ' -f1)" = "$BASE_SHA" ] || { rm -f "$BASE"; echo "$BASE_URL: checksum differs" >&2; exit 1; }
fi

IN="$OUT/in"; rm -rf "$IN"; mkdir -p "$IN/pkg"
[ -n "$SSHKEY" ] && cp "$SSHKEY" "$IN/authorized_keys"
REPO_URL="${PSPEXP_REPO_URL:-$(head -1 "$HOME/.pspexp/repo-url" 2>/dev/null)}"
[ -n "$REPO_URL" ] || REPO_URL=$(head -1 "$ROOT/scripts/packages/public-repo-url" 2>/dev/null)   # else the public repository
[ -n "$REPO_URL" ] || { echo "no package repository address (\$PSPEXP_REPO_URL or ~/.pspexp/repo-url)" >&2; exit 1; }
echo "$REPO_URL" > "$IN/pkg/repo-url"
gpg --dearmor < "$ROOT/scripts/packages/pspexp-archive-key.asc" > "$IN/pkg/keyring.gpg"
cp "$ROOT/scripts/board/pspexp-apt-setup" "$ROOT/scripts/board/pspexp-pkg-apt" "$IN/pkg/"
cp "$ROOT/scripts/pi/board-files.sh" "$ROOT/scripts/pi/image/image-setup.sh" "$IN/"

# a .local repository name resolves here (mDNS), not in the container: pass its address
RH=$(echo "$REPO_URL" | sed -E 's#^[a-z]+://([^:/]+).*#\1#'); ADDHOST=""
case "$RH" in *.local)
	RIP=$(dscacheutil -q host -a name "$RH" 2>/dev/null | awk '/^ip_address/ {print $2; exit}')
	[ -n "$RIP" ] || { echo "cannot resolve $RH" >&2; exit 1; }
	ADDHOST="--add-host=$RH:$RIP" ;;
esac
docker volume create pspexp-pi-imgbuild >/dev/null
# (PSPEXP_PLATFORM: the container's platform; linux/arm64 on the Mac, linux/amd64 in CI with QEMU)
docker run --rm --privileged --platform "${PSPEXP_PLATFORM:-linux/arm64}" $ADDHOST \
	-v pspexp-pi-imgbuild:/work -v "$OUT":/out debian:trixie sh /out/in/image-setup.sh
ls -la "$OUT/pspexp-pi.img"
