# Everything PSPExp installs on a ClockworkPi GameShell, in one place, for
# both installers: scripts/gameshell-prep-image.sh (writes it into an image
# before flashing) and scripts/install-gameshell.sh (a running GameShell over
# SSH). Sourced, not run. The caller defines
#   gs_fetch PATH OUT   copy the target's PATH to local OUT (empty if missing)
# and calls gs_stage DIR, which fills DIR/root/<path> with the files and
# writes DIR/manifest, one entry per line, in order:
#   dir  MODE UID GID PATH      a directory (created when missing)
#   file MODE UID GID PATH      DIR/root/PATH installed at PATH
#   link TARGET PATH            a symbolic link
#   rm   PATH                   removed if present
# Mode, uid and gid are octal / numeric (cpi is 1000). gs_stage sets the
# globals S R M B G: callers keep their own names clear of them.

GS_TYRIAN_DIR="/home/cpi/apps/Menu/21_Indie Games/34_OpenTyrian"

gs_stage() {
	S="$1"; R="$S/root"; M="$S/manifest"
	B="$ROOT/build/gameshell"; G="$ROOT/scripts/gameshell"
	rm -rf "$S"; mkdir -p "$R"; : > "$M"
	[ -x "$B/bin/pspexp-term" ] || { echo "run scripts/build-gameshell.sh first" >&2; return 1; }
	[ -f "$B/lib/libsdl-pspexp.so" ] || { echo "run scripts/build-gameshell.sh (the shim) first" >&2; return 1; }
	gs_dir 0755 0 0 /etc/pspexp
	gs_dir 0755 1000 1000 /home/cpi/pspexp

	# tools, the SDL layer and its run wrappers
	for f in pspexp-term pspexp-bench pspexp-sprite-demo; do gs_copy "$B/bin/$f" 0755 0 0 "/usr/local/bin/$f"; done
	gs_copy "$ROOT/scripts/pspexp-exec" 0755 0 0 /usr/local/bin/pspexp-exec
	gs_copy "$B/lib/libsdl-pspexp.so" 0644 0 0 /usr/local/lib/libsdl-pspexp.so
	gs_copy "$G/pspexp-run" 0755 0 0 /usr/local/bin/pspexp-run
	gs_copy "$G/pspexp-tyrian" 0755 0 0 /usr/local/bin/pspexp-tyrian
	# OpenTyrian (SDL2) is compiled on a GameShell (build-gameshell.sh tyrian),
	# which keeps a copy here for the next card
	[ -x "$B/bin/opentyrian" ] && gs_copy "$B/bin/opentyrian" 0755 1000 1000 /home/cpi/pspexp/opentyrian

	# the console at boot: the launcher mirrored, the PSP's buttons as its keypad
	cat > "$S/pspexp-term.service" <<U
[Unit]
Description=PSPExp console on the PSP (pspexp-term)
After=multi-user.target

[Service]
User=cpi
WorkingDirectory=/home/cpi
Environment=PSPEXP_TERM=1 HOME=/home/cpi SHELL=/usr/bin/bash PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
ExecStart=/usr/local/bin/pspexp-term --shell /usr/bin/bash --mirror /dev/fb0 --usb --tcp 192.168.50.2
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
U
	gs_copy "$S/pspexp-term.service" 0644 0 0 /etc/systemd/system/pspexp-term.service
	gs_link /etc/systemd/system/pspexp-term.service /etc/systemd/system/multi-user.target.wants/pspexp-term.service

	# USB access to the PSP, the virtual keypad, the audio FIFO, ALSA into it
	{
		echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="054c", ATTR{idProduct}=="01ca", MODE="0666"'
		echo 'KERNEL=="uinput", MODE="0660", GROUP="input", OPTIONS+="static_node=uinput"'
	} > "$S/60-pspexp.rules"
	gs_copy "$S/60-pspexp.rules" 0644 0 0 /etc/udev/rules.d/60-pspexp.rules
	echo uinput > "$S/pspexp-modules.conf"
	gs_copy "$S/pspexp-modules.conf" 0644 0 0 /etc/modules-load.d/pspexp.conf
	echo 'p /tmp/pspexp-audio 0666 cpi cpi -' > "$S/pspexp-tmpfiles.conf"
	gs_copy "$S/pspexp-tmpfiles.conf" 0644 0 0 /etc/tmpfiles.d/pspexp.conf
	cat > "$S/asound.conf" <<A
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
	gs_copy "$S/asound.conf" 0644 0 0 /etc/asound.conf

	# Wi-Fi mode (psp: the GameShell is the PSP's access point; home) and the
	# USB network that carries the outside world in PSP mode. The mode itself
	# (/etc/pspexp/wifi-mode) is left as it is: home when missing.
	gs_copy "$G/pspexp-wifi-mode" 0755 0 0 /usr/local/bin/pspexp-wifi-mode
	gs_copy "$G/hostapd.conf" 0600 0 0 /etc/pspexp/hostapd.conf
	gs_copy "$G/pspexp-wifi-mode.service" 0644 0 0 /etc/systemd/system/pspexp-wifi-mode.service
	gs_link /etc/systemd/system/pspexp-wifi-mode.service /etc/systemd/system/multi-user.target.wants/pspexp-wifi-mode.service
	gs_fetch /etc/network/interfaces.d/usb "$S/usb.orig"
	if [ -s "$S/usb.orig" ] && ! grep -q PSPExp "$S/usb.orig"; then
		gs_copy "$S/usb.orig" 0644 0 0 /etc/network/interfaces.d/usb.pspexp-bak
	fi
	gs_copy "$G/usb0.interfaces" 0644 0 0 /etc/network/interfaces.d/usb

	# The launcher's X session: fbdev (fbturbo is not installed, and the
	# modesetting fallback never draws into /dev/fb0, which the mirror reads);
	# without ~/.lima it uses that config. Its start in ~/.bashrc skips the
	# PSP console's own login shell (PSPEXP_TERM=1), which would otherwise
	# take the one-time autologin flag and try to run X on a pseudo-terminal.
	gs_fetch /home/cpi/launchergo/.xorg.conf "$S/xorg.conf"
	if [ -s "$S/xorg.conf" ] && grep -q fbturbo "$S/xorg.conf"; then
		sed 's/Driver *"fbturbo"/Driver          "fbdev"/' "$S/xorg.conf" > "$S/xorg.conf.new"
		gs_copy "$S/xorg.conf.new" 0644 1000 1000 /home/cpi/launchergo/.xorg.conf
	fi
	gs_rm /home/cpi/.lima
	gs_fetch /home/cpi/.bashrc "$S/bashrc"
	if [ -s "$S/bashrc" ] && ! grep -q PSPEXP_TERM "$S/bashrc"; then
		sed 's|^if \[ -f ~/launchergo/.cpirc \]; then|# PSPExp: the PSP console'"'"'s shell (PSPEXP_TERM=1) must not start the X session\
if [ -f ~/launchergo/.cpirc ] \&\& [ -z "$PSPEXP_TERM" ]; then|' "$S/bashrc" > "$S/bashrc.new"
		gs_copy "$S/bashrc.new" 0644 1000 1000 /home/cpi/.bashrc
	fi

	# The launcher's OpenTyrian entry: the SDL2 build on the PSP while the
	# console runs (the stock one is SDL 1.2, out of the SDL layer's reach),
	# the stock build otherwise; the stock script is kept as .orig
	gs_fetch "$GS_TYRIAN_DIR/OpenTyrian.sh" "$S/tyrian.sh"
	# already PSPExp's: rebuilt from the stock copy (a newer PSPExp version)
	grep -q pspexp "$S/tyrian.sh" 2>/dev/null && gs_fetch "$GS_TYRIAN_DIR/OpenTyrian.sh.orig" "$S/tyrian.sh"
	if [ -s "$S/tyrian.sh" ] && ! grep -q pspexp "$S/tyrian.sh"; then
		gs_copy "$S/tyrian.sh" 0755 1000 1000 "$GS_TYRIAN_DIR/OpenTyrian.sh.orig"
		{
			echo '# PSPExp: with the PSP console running, the SDL2 build on the PSP (pspexp-tyrian);'
			echo '# otherwise the stock SDL 1.2 build (this script before PSPExp: OpenTyrian.sh.orig)'
			echo 'if [ -f /tmp/pspexp-term.pid ] && kill -0 $(cat /tmp/pspexp-term.pid) 2>/dev/null && [ -x /home/cpi/pspexp/opentyrian ]; then'
			echo '	exec /usr/local/bin/pspexp-tyrian'
			echo 'fi'
			cat "$S/tyrian.sh"
		} > "$S/tyrian.sh.new"
		gs_copy "$S/tyrian.sh.new" 0755 1000 1000 "$GS_TYRIAN_DIR/OpenTyrian.sh"
	fi
}

gs_dir()  { echo "dir $1 $2 $3 $4" >> "$M"; }
gs_copy() { mkdir -p "$R$(dirname "$5")"; cp "$1" "$R$5"; echo "file $2 $3 $4 $5" >> "$M"; }
gs_link() { echo "link $1 $2" >> "$M"; }
gs_rm()   { echo "rm $1" >> "$M"; }
