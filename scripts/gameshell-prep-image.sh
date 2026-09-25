#!/bin/sh
# Prepare a ClockworkPi GameShell OS image (v0.6, Armbian jammy) on this Mac
# before flashing, for a board with no screen:
#   - this Mac's SSH key for the cpi user
#   - pspexp-term (the PSP as the console) as a systemd service for cpi,
#     plus pspexp-exec, pspexp-bench and pspexp-sprite-demo in /usr/local/bin
#   - the sdl-pspexp shim (/usr/local/lib/libsdl-pspexp.so) and pspexp-run, so
#     the image's own SDL2 games can use the PSP directly (freeDM: see docs)
#   - a udev rule so cpi can open the PSP over USB and create the virtual
#     keypad (/dev/uinput), and uinput loaded at boot
#   - mirror view: the console starts showing the GameShell's own screen
#     (/dev/fb0: the launcher and games) with the PSP's buttons as its keypad
#   - ALSA's default output goes to the console's audio FIFO (the PSP plays it)
#   - with --wifi: a NetworkManager connection; prompts for the network name
#     and password (they go into the image only)
#
#   scripts/build-gameshell.sh
#   scripts/gameshell-prep-image.sh clockworkos_v0.6.img [--wifi]
# Needs e2fsprogs (brew install e2fsprogs) for debugfs.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="$1"; shift || true
[ -f "$IMG" ] || { echo "usage: $0 image.img [--wifi]" >&2; exit 2; }
DF="$(brew --prefix e2fsprogs 2>/dev/null)/sbin/debugfs"
[ -x "$DF" ] || { echo "debugfs not found: brew install e2fsprogs" >&2; exit 1; }
BIN="$ROOT/build/gameshell/bin"
[ -x "$BIN/pspexp-term" ] || { echo "run scripts/build-gameshell.sh first" >&2; exit 1; }
# offset of the Linux root partition (MBR partition 2)
OFF=$(python3 -c "import struct,sys; d=open(sys.argv[1],'rb').read(512); print(struct.unpack('<I', d[446+16+8:446+16+12])[0]*512)" "$IMG")
FS="$IMG?offset=$OFF"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
CMDS="$T/cmds"
: > "$CMDS"
put() {   # put LOCAL REMOTE MODE UID GID
	echo "rm $2" >> "$CMDS"
	echo "write $1 $2" >> "$CMDS"
	echo "set_inode_field $2 mode $3" >> "$CMDS"
	echo "set_inode_field $2 uid $4" >> "$CMDS"
	echo "set_inode_field $2 gid $5" >> "$CMDS"
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
	# tools
	for f in pspexp-term pspexp-bench pspexp-sprite-demo; do put "$BIN/$f" "/usr/local/bin/$f" 0100755 0 0; done
	put "$ROOT/scripts/pspexp-exec" /usr/local/bin/pspexp-exec 0100755 0 0
	# the sdl-pspexp shim for the image's own SDL2 games and its run wrapper
	if [ -f "$ROOT/build/gameshell/lib/libsdl-pspexp.so" ]; then
		put "$ROOT/build/gameshell/lib/libsdl-pspexp.so" /usr/local/lib/libsdl-pspexp.so 0100644 0 0
		put "$ROOT/scripts/gameshell/pspexp-run" /usr/local/bin/pspexp-run 0100755 0 0
	fi
	# service
	cat > "$T/pspexp-term.service" <<U
[Unit]
Description=PSPExp console on the PSP (pspexp-term)
After=multi-user.target

[Service]
User=cpi
WorkingDirectory=/home/cpi
Environment=PSPEXP_TERM=1 HOME=/home/cpi SHELL=/usr/bin/bash PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
ExecStart=/usr/local/bin/pspexp-term --shell /usr/bin/bash --mirror /dev/fb0
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
U
	put "$T/pspexp-term.service" /etc/systemd/system/pspexp-term.service 0100644 0 0
	echo "rm /etc/systemd/system/multi-user.target.wants/pspexp-term.service" >> "$CMDS"
	echo "symlink /etc/systemd/system/multi-user.target.wants/pspexp-term.service /etc/systemd/system/pspexp-term.service" >> "$CMDS"
	{
		echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="054c", ATTR{idProduct}=="01ca", MODE="0666"'
		echo 'KERNEL=="uinput", MODE="0660", GROUP="input", OPTIONS+="static_node=uinput"'
	} > "$T/60-pspexp.rules"
	put "$T/60-pspexp.rules" /etc/udev/rules.d/60-pspexp.rules 0100644 0 0
	echo uinput > "$T/pspexp-modules.conf"
	put "$T/pspexp-modules.conf" /etc/modules-load.d/pspexp.conf 0100644 0 0
	# the audio FIFO exists from boot, owned by cpi (the console reads it)
	echo 'p /tmp/pspexp-audio 0666 cpi cpi -' > "$T/pspexp-tmpfiles.conf"
	put "$T/pspexp-tmpfiles.conf" /etc/tmpfiles.d/pspexp.conf 0100644 0 0
	cat > "$T/asound.conf" <<A
# PSPExp: the default output is 44.1 kHz S16 stereo raw into the FIFO that
# pspexp-term plays on the PSP (the pipe paces the writer); the mixer controls
# stay on the board's own codec.
pcm.!default {
	type plug
	slave { pcm "pspexp_out"; format S16_LE; rate 44100; channels 2 }
}
pcm.pspexp_out {
	type file
	slave.pcm "null"
	file "/tmp/pspexp-audio"
	format "raw"
}
ctl.!default { type hw; card 0 }
A
	put "$T/asound.conf" /etc/asound.conf 0100644 0 0
	# The launcher's X session: fbdev (fbturbo is not installed, and the
	# modesetting fallback never draws into /dev/fb0, which the mirror reads);
	# without ~/.lima it uses that config. Its start in ~/.bashrc skips the
	# PSP console's own login shell (PSPEXP_TERM=1), which would otherwise
	# take the one-time autologin flag and try to run X on a pseudo-terminal.
	"$DF" -R "cat /home/cpi/launchergo/.xorg.conf" "$FS" 2>/dev/null | sed 's/Driver *"fbturbo"/Driver          "fbdev"/' > "$T/xorg.conf"
	[ -s "$T/xorg.conf" ] && put "$T/xorg.conf" /home/cpi/launchergo/.xorg.conf 0100644 1000 1000
	echo "rm /home/cpi/.lima" >> "$CMDS"
	"$DF" -R "cat /home/cpi/.bashrc" "$FS" 2>/dev/null > "$T/bashrc"
	if [ -s "$T/bashrc" ] && ! grep -q PSPEXP_TERM "$T/bashrc"; then
		sed -i.bak 's|^if \[ -f ~/launchergo/.cpirc \]; then|# PSPExp: the PSP console'"'"'s shell (PSPEXP_TERM=1) must not start the X session\
if [ -f ~/launchergo/.cpirc ] \&\& [ -z "$PSPEXP_TERM" ]; then|' "$T/bashrc"
		put "$T/bashrc" /home/cpi/.bashrc 0100644 1000 1000
	fi
fi
# rm only names that exist (debugfs rm of a missing name is harmless, but keep it quiet)
"$DF" -w -f "$CMDS" "$FS" 2>&1 | grep -viE "^debugfs [0-9]|File not found by ext2_lookup|Ext2 directory already exists|^rm |^write |^set_inode|^mkdir |^symlink |^Allocated" || true
E2FSCK="$(brew --prefix e2fsprogs)/sbin/e2fsck"
if "$E2FSCK" -fn "$FS" >"$T/fsck" 2>&1; then echo "prepared: $IMG (filesystem check clean)"
else echo "filesystem check FAILED after editing $IMG:" >&2; grep -vE "^Pass" "$T/fsck" | head -20 >&2; exit 1; fi
