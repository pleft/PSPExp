# The GameShell's own PSPExp files, in one place, for both installers: the
# card image (scripts/gameshell/base/build-image.sh) and scripts/gameshell/install.sh
# (a running GameShell over SSH). With GS_CORE=apt, PSPExp itself is the
# pspexp-core package and this is only what is the GameShell's own; otherwise
# (no package repository) the tools too, into /usr/local. Sourced, not run.
# The caller calls gs_stage DIR, which fills DIR/root/<path> with the files and
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

	# tools, the SDL layer and its run wrappers. GS_CORE=apt: they come from the
	# pspexp-core package instead (/usr/bin, /usr/lib/pspexp, /usr/share/pspexp),
	# and the copies here go, since /usr/local would shadow the package's
	if [ "${GS_CORE:-}" = apt ]; then
		for f in pspexp-term pspexp-bench pspexp-sprite-demo pspexp-exec pspexp-run pspexp-pkg; do gs_rm "/usr/local/bin/$f"; done
		gs_rm /usr/local/lib/libsdl-pspexp.so
		gs_rm /usr/local/share/pspexp/launcher
		CONSOLE=/usr/bin/pspexp-term
	else
		for f in pspexp-term pspexp-bench pspexp-sprite-demo; do gs_copy "$B/bin/$f" 0755 0 0 "/usr/local/bin/$f"; done
		gs_copy "$ROOT/scripts/board/pspexp-exec" 0755 0 0 /usr/local/bin/pspexp-exec
		gs_copy "$B/lib/libsdl-pspexp.so" 0644 0 0 /usr/local/lib/libsdl-pspexp.so
		gs_copy "$ROOT/scripts/board/pspexp-run" 0755 0 0 /usr/local/bin/pspexp-run
		CONSOLE=/usr/local/bin/pspexp-term
	fi
	gs_rm /usr/local/bin/pspexp-tyrian   # replaced by the opentyrian package

	# PSPExp packages: the launcher's games and apps (scripts/packages/pspexp-pkg-build on the
	# Mac, pspexp-pkg here), owned by cpi so installing needs no root
	gs_dir 0755 1000 1000 /opt/pspexp
	gs_dir 0755 1000 1000 /opt/pspexp/apps
	gs_dir 0755 1000 1000 /opt/pspexp/systems
	[ "${GS_CORE:-}" = apt ] || gs_copy "$ROOT/scripts/board/pspexp-pkg" 0755 0 0 /usr/local/bin/pspexp-pkg
	# the card's first boot: the root partition grown to the card; once
	gs_rm /usr/local/lib/pspexp/clean-stock.sh   # ClockworkOS's clean-up, gone with ClockworkOS
	gs_rm /opt/pspexp/preinstall                  # bundled archives, gone with them
	gs_copy "$G/pspexp-firstboot" 0755 0 0 /usr/local/bin/pspexp-firstboot
	gs_copy "$G/pspexp-firstboot.service" 0644 0 0 /etc/systemd/system/pspexp-firstboot.service
	gs_link /etc/systemd/system/pspexp-firstboot.service /etc/systemd/system/multi-user.target.wants/pspexp-firstboot.service

	# the launcher's fonts and icons (Inter, Lucide: open licences, kept beside
	# them) and the commands its System page may run without a password
	if [ "${GS_CORE:-}" != apt ]; then
		gs_dir 0755 0 0 /usr/local/share/pspexp
		gs_dir 0755 0 0 /usr/local/share/pspexp/launcher
		for f in Inter-Regular.ttf Inter-SemiBold.ttf lucide.ttf Inter-LICENSE.txt lucide-LICENSE.txt; do
			gs_copy "$ROOT/tools/pspexp-term/assets/$f" 0644 0 0 "/usr/local/share/pspexp/launcher/$f"
		done
	fi
	# nmcli joins Wi-Fi with the password among its arguments: sudo must not log those
	{ echo 'cpi ALL=(root) NOPASSWD: /usr/sbin/reboot, /usr/sbin/poweroff, /usr/local/bin/pspexp-wifi-mode psp, /usr/local/bin/pspexp-wifi-mode home, /usr/bin/timedatectl set-timezone *, /usr/bin/nmcli, /usr/sbin/rfkill unblock bluetooth'
	  echo 'Defaults!/usr/bin/nmcli !log_allowed'; } > "$S/pspexp.sudoers"
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
ExecStart=$CONSOLE --shell /usr/bin/bash --launcher --mirror /dev/fb0 --usb --tcp 192.168.50.2
Restart=always
RestartSec=2
# real-time priority allowed: its programs' sound to Bluetooth (pw-play) starved without it on the GameShell
LimitRTPRIO=95
LimitNICE=-19
LimitMEMLOCK=4194304

[Install]
WantedBy=multi-user.target
U
	gs_copy "$S/pspexp-term.service" 0644 0 0 /etc/systemd/system/pspexp-term.service
	gs_link /etc/systemd/system/pspexp-term.service /etc/systemd/system/multi-user.target.wants/pspexp-term.service
	# the user's PipeWire (no login: lingering) may take real-time priority, as
	# PipeWire's own 25-pw-rlimits.conf asks for its group, which no login sets here
	printf '%s\n' '[Service]' 'LimitRTPRIO=95' 'LimitNICE=-19' 'LimitMEMLOCK=4194304' > "$S/pspexp-rt.conf"
	gs_copy "$S/pspexp-rt.conf" 0644 0 0 /etc/systemd/system/user@.service.d/pspexp-rt.conf

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
	gs_rm /etc/network/interfaces.d/usb.pspexp-bak
	gs_copy "$G/usb0.interfaces" 0644 0 0 /etc/network/interfaces.d/usb
}

gs_dir()  { echo "dir $1 $2 $3 $4" >> "$M"; }
gs_copy() { mkdir -p "$R$(dirname "$5")"; cp "$1" "$R$5"; echo "file $2 $3 $4 $5" >> "$M"; }
gs_link() { echo "link $1 $2" >> "$M"; }
gs_rm()   { echo "rm $1" >> "$M"; }
