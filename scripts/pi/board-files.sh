#!/bin/sh
# The Pi's own files for PSPExp, as root: the package folders, the sudo rule for
# the System page, the PSP's USB rule and the console's service (enabled, not
# started). The same for a running Pi (scripts/pi/install.sh, which then restarts
# the console) and for the card image (scripts/pi/image/image-setup.sh, in a chroot).
#   board-files.sh USER CONSOLE      (CONSOLE: /usr/bin/pspexp-term, or /usr/local/bin/... the older way)
set -e
U="$1"; CONSOLE="$2"
[ -n "$CONSOLE" ] || { echo "usage: board-files.sh USER CONSOLE" >&2; exit 2; }
H=$(getent passwd "$U" | cut -d: -f6)
SH=$(getent passwd "$U" | cut -d: -f7)
install -d -m 0755 /etc/pspexp
install -d -m 0755 -o "$U" -g "$U" /opt/pspexp /opt/pspexp/apps /opt/pspexp/systems
# nmcli joins Wi-Fi with the password among its arguments: sudo must not log those
printf '%s\n' "$U ALL=(root) NOPASSWD: /usr/sbin/reboot, /usr/sbin/poweroff, /usr/bin/timedatectl set-timezone *, /usr/bin/nmcli, /usr/sbin/rfkill unblock bluetooth" 'Defaults!/usr/bin/nmcli !log_allowed' > /etc/sudoers.d/pspexp
chmod 0440 /etc/sudoers.d/pspexp
# the user's PipeWire runs without a login (Bluetooth sound, pspexp-bt: the console is a
# system service); as loginctl enable-linger does, which a chroot has no logind for
install -d -m 0755 /var/lib/systemd/linger && touch "/var/lib/systemd/linger/$U"
install -d -m 0755 /etc/udev/rules.d   # (a system without udev's own rules yet: a container, a trimmed install)
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="054c", ATTR{idProduct}=="01c[ab]", MODE="0666"' > /etc/udev/rules.d/60-pspexp.rules
cat > /etc/systemd/system/pspexp-term.service <<U
[Unit]
Description=PSPExp console on the PSP (pspexp-term)
After=multi-user.target

[Service]
User=$U
WorkingDirectory=$H
Environment=PSPEXP_TERM=1 HOME=$H SHELL=${SH:-/bin/bash} PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:/usr/games
ExecStart=$CONSOLE --shell ${SH:-/bin/bash} --launcher --usb
Restart=always
RestartSec=2
# real-time priority allowed: its programs' sound to Bluetooth (pw-play) starved without it on the GameShell
LimitRTPRIO=95
LimitNICE=-19
LimitMEMLOCK=4194304

[Install]
WantedBy=multi-user.target
U
# the user's PipeWire may take real-time priority with nobody logged in (Bluetooth sound)
install -d -m 0755 /etc/systemd/system/user@.service.d
printf '%s\n' '[Service]' 'LimitRTPRIO=95' 'LimitNICE=-19' 'LimitMEMLOCK=4194304' > /etc/systemd/system/user@.service.d/pspexp-rt.conf
systemctl enable pspexp-term >/dev/null 2>&1
