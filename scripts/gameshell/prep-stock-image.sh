#!/bin/sh
# Prepare a ClockworkPi GameShell OS image (v0.6, Armbian jammy) on this Mac
# before flashing, for a board with no screen:
#   - this Mac's SSH key for the cpi user
#   - everything in scripts/gameshell/stage.sh (the same list
#     scripts/gameshell/install.sh installs on a running GameShell): the
#     tools, the SDL layer with pspexp-run / pspexp-tyrian, the console
#     service (launcher mirrored, PSP buttons as its keypad), udev rules,
#     audio into the PSP, the Wi-Fi mode switch, the launcher's X and
#     OpenTyrian entries
#   - with --wifi: a NetworkManager connection; prompts for the network name
#     and password (they go into the image only)
#
#   scripts/gameshell/build.sh
#   scripts/gameshell/prep-stock-image.sh clockworkos_v0.6.img [--wifi]
# Needs e2fsprogs (brew install e2fsprogs) for debugfs.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IMG="$1"; shift || true
[ -f "$IMG" ] || { echo "usage: $0 image.img [--wifi]" >&2; exit 2; }
DF="$(brew --prefix e2fsprogs 2>/dev/null)/sbin/debugfs"
[ -x "$DF" ] || { echo "debugfs not found: brew install e2fsprogs" >&2; exit 1; }
BIN="$ROOT/build/gameshell/bin"
[ -x "$BIN/pspexp-term" ] || { echo "run scripts/gameshell/build.sh first" >&2; exit 1; }
# offset of the Linux root partition (MBR partition 2)
OFF=$(python3 -c "import struct,sys; d=open(sys.argv[1],'rb').read(512); print(struct.unpack('<I', d[446+16+8:446+16+12])[0]*512)" "$IMG")
FS="$IMG?offset=$OFF"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
CMDS="$T/cmds"
: > "$CMDS"
put() {   # put LOCAL REMOTE MODE UID GID
	echo "rm \"$2\"" >> "$CMDS"
	echo "write \"$1\" \"$2\"" >> "$CMDS"
	echo "set_inode_field \"$2\" mode $3" >> "$CMDS"
	echo "set_inode_field \"$2\" uid $4" >> "$CMDS"
	echo "set_inode_field \"$2\" gid $5" >> "$CMDS"
}

if [ "$1" = "--wifi" ]; then
	printf "Wi-Fi network name: "; read -r SSID
	printf "Wi-Fi password (not shown): "; stty -echo; read -r PSK; stty echo; echo
	UUID=$(uuidgen | tr 'A-Z' 'a-z')
	cat > "$T/wifi.nmconnection" <<W
[connection]
id=$SSID
uuid=$UUID
type=wifi
autoconnect=true

[wifi]
mode=infrastructure
ssid=$SSID

[wifi-security]
key-mgmt=wpa-psk
psk=$PSK

[ipv4]
method=auto

[ipv6]
method=auto
W
	put "$T/wifi.nmconnection" "/etc/NetworkManager/system-connections/pspexp-wifi.nmconnection" 0100600 0 0
	PSK=
else
	# SSH key (kept with whatever the image already had)
	"$DF" -R "cat /home/cpi/.ssh/authorized_keys" "$FS" 2>/dev/null > "$T/authorized_keys" || true
	for k in "$HOME"/.ssh/id_ed25519.pub "$HOME"/.ssh/id_rsa.pub; do
		[ -f "$k" ] && ! grep -qF "$(cut -d' ' -f2 "$k")" "$T/authorized_keys" && cat "$k" >> "$T/authorized_keys"
	done
	# mkdir only when missing: debugfs mkdir on an existing name half-creates
	# an orphan directory and later hands its block to another file
	if ! "$DF" -R "stat /home/cpi/.ssh" "$FS" 2>/dev/null | grep -q "Type: directory"; then
		echo "mkdir /home/cpi/.ssh" >> "$CMDS"
	fi
	echo "set_inode_field /home/cpi/.ssh mode 040700" >> "$CMDS"
	echo "set_inode_field /home/cpi/.ssh uid 1000" >> "$CMDS"
	echo "set_inode_field /home/cpi/.ssh gid 1000" >> "$CMDS"
	put "$T/authorized_keys" /home/cpi/.ssh/authorized_keys 0100600 1000 1000
	# the PSPExp files (the list is shared with install-gameshell.sh)
	. "$ROOT/scripts/gameshell/stage.sh"
	gs_fetch() { "$DF" -R "cat \"$1\"" "$FS" > "$2" 2>/dev/null || : ; }
	gs_stage "$T/stage"
	while read -r kind a b c d; do
		case $kind in
		dir)
			# mkdir only when missing (as for .ssh above)
			"$DF" -R "stat \"$d\"" "$FS" 2>/dev/null | grep -q "Type: directory" || echo "mkdir \"$d\"" >> "$CMDS"
			echo "set_inode_field \"$d\" mode 04$a" >> "$CMDS"
			echo "set_inode_field \"$d\" uid $b" >> "$CMDS"
			echo "set_inode_field \"$d\" gid $c" >> "$CMDS" ;;
		file) put "$T/stage/root$d" "$d" "010$a" "$b" "$c" ;;
		link) echo "rm $b" >> "$CMDS"; echo "symlink $b $a" >> "$CMDS" ;;
		rm)   echo "rm $a" >> "$CMDS" ;;
		esac
	done < "$T/stage/manifest"
fi
# rm only names that exist (debugfs rm of a missing name is harmless, but keep it quiet)
"$DF" -w -f "$CMDS" "$FS" 2>&1 | grep -viE "^debugfs [0-9]|^debugfs: |^$|File not found by ext2_lookup|Ext2 directory already exists|^rm |^write |^set_inode|^mkdir |^symlink |^Allocated" || true
E2FSCK="$(brew --prefix e2fsprogs)/sbin/e2fsck"
if "$E2FSCK" -fn "$FS" >"$T/fsck" 2>&1; then echo "prepared: $IMG (filesystem check clean)"
else echo "filesystem check FAILED after editing $IMG:" >&2; grep -vE "^Pass" "$T/fsck" | head -20 >&2; exit 1; fi
