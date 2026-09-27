# Everything PSPExp installs on a ClockworkPi GameShell, in one place, for
# both installers: scripts/gameshell/prep-stock-image.sh (writes it into an image
# before flashing) and scripts/gameshell/install.sh (a running GameShell over
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

gs_stage() {
	S="$1"; R="$S/root"; M="$S/manifest"
	B="$ROOT/build/gameshell"; G="$ROOT/scripts/gameshell"
	rm -rf "$S"; mkdir -p "$R"; : > "$M"
	[ -x "$B/bin/pspexp-term" ] || { echo "run scripts/gameshell/build.sh first" >&2; return 1; }
	[ -f "$B/lib/libsdl-pspexp.so" ] || { echo "run scripts/gameshell/build.sh (the shim) first" >&2; return 1; }
	gs_dir 0755 0 0 /etc/pspexp
	gs_dir 0755 1000 1000 /home/cpi/pspexp

	# tools, the SDL layer and its run wrappers
	for f in pspexp-term pspexp-bench pspexp-sprite-demo; do gs_copy "$B/bin/$f" 0755 0 0 "/usr/local/bin/$f"; done
	gs_copy "$ROOT/scripts/board/pspexp-exec" 0755 0 0 /usr/local/bin/pspexp-exec
	gs_copy "$B/lib/libsdl-pspexp.so" 0644 0 0 /usr/local/lib/libsdl-pspexp.so
	gs_copy "$ROOT/scripts/board/pspexp-run" 0755 0 0 /usr/local/bin/pspexp-run
	gs_rm /usr/local/bin/pspexp-tyrian   # replaced by the opentyrian package

	# PSPExp packages: the launcher's games and apps (scripts/packages/pspexp-pkg-build on the
	# Mac, pspexp-pkg here), owned by cpi so installing needs no root
	gs_dir 0755 1000 1000 /opt/pspexp
	gs_dir 0755 1000 1000 /opt/pspexp/apps
	gs_dir 0755 1000 1000 /opt/pspexp/systems
	gs_copy "$ROOT/scripts/board/pspexp-pkg" 0755 0 0 /usr/local/bin/pspexp-pkg
	# the card's first boot: the stock launcher and games removed, the bundled
	# packages (every build/gameshell/pkg/*.tar.gz) installed; once
	gs_dir 0755 0 0 /usr/local/lib/pspexp
	gs_copy "$G/clean-stock.sh" 0755 0 0 /usr/local/lib/pspexp/clean-stock.sh
	gs_copy "$G/pspexp-firstboot" 0755 0 0 /usr/local/bin/pspexp-firstboot
	gs_copy "$G/pspexp-firstboot.service" 0644 0 0 /etc/systemd/system/pspexp-firstboot.service
	gs_link /etc/systemd/system/pspexp-firstboot.service /etc/systemd/system/multi-user.target.wants/pspexp-firstboot.service
	if [ "${GS_PREINSTALL:-1}" = 1 ]; then
		gs_dir 0755 1000 1000 /opt/pspexp/preinstall
		for p in "$B"/pkg/*.tar.gz; do
			[ -f "$p" ] && gs_copy "$p" 0644 1000 1000 "/opt/pspexp/preinstall/$(basename "$p")"
		done
	fi

	# the launcher's fonts and icons (Inter, Lucide: open licences, kept beside
	# them) and the commands its System page may run without a password
	gs_dir 0755 0 0 /usr/local/share/pspexp
	gs_dir 0755 0 0 /usr/local/share/pspexp/launcher
	for f in Inter-Regular.ttf Inter-SemiBold.ttf lucide.ttf Inter-LICENSE.txt lucide-LICENSE.txt; do
		gs_copy "$ROOT/tools/pspexp-term/assets/$f" 0644 0 0 "/usr/local/share/pspexp/launcher/$f"
	done
	echo 'cpi ALL=(root) NOPASSWD: /usr/sbin/reboot, /usr/sbin/poweroff, /usr/local/bin/pspexp-wifi-mode psp, /usr/local/bin/pspexp-wifi-mode home' > "$S/pspexp.sudoers"
	gs_copy "$S/pspexp.sudoers" 0440 0 0 /etc/sudoers.d/pspexp

	# the console at boot: the PSPExp launcher as the home view, showing the
	# installed packages (/opt/pspexp) and the user's own entries
	cat > "$S/pspexp-term.service" <<U
[Unit]
Description=PSPExp console on the PSP (pspexp-term)
After=multi-user.target

[Service]
User=cpi
WorkingDirectory=/home/cpi
Environment=PSPEXP_TERM=1 HOME=/home/cpi SHELL=/usr/bin/bash PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:/usr/games
ExecStart=/usr/local/bin/pspexp-term --shell /usr/bin/bash --launcher --mirror /dev/fb0 --usb --tcp 192.168.50.2
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
	# (no uinput: this kernel has no modules; the keypad used X, which is gone)
	gs_rm /etc/modules-load.d/pspexp.conf
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
		# kept outside interfaces.d: ifupdown reads every file there, and the
		# stock stanza's "auto usb0" raced ours at boot
		gs_copy "$S/usb.orig" 0644 0 0 /etc/pspexp/usb.interfaces.stock
	fi
	gs_rm /etc/network/interfaces.d/usb.pspexp-bak
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
}

gs_dir()  { echo "dir $1 $2 $3 $4" >> "$M"; }
gs_copy() { mkdir -p "$R$(dirname "$5")"; cp "$1" "$R$5"; echo "file $2 $3 $4 $5" >> "$M"; }
gs_link() { echo "link $1 $2" >> "$M"; }
gs_rm()   { echo "rm $1" >> "$M"; }
