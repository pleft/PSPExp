#!/bin/sh
# Once, on a GameShell running ClockworkOS: remove its stock launcher, games,
# emulators and apps, leaving the system, Wi-Fi, sound, SSH and everything
# PSPExp installs. After this the PSP launcher shows only PSPExp packages
# (scripts/gs-pkg). Run as root, by pspexp-firstboot on a card's first boot;
# by hand: ssh cpi@gameshell 'sudo sh -s' < scripts/gameshell/clean-stock.sh
# Keeps: chocolate-doom (the freedoom package uses it), Xorg (installed, not
# started), ~/pspexp (build trees), ~/.config/pspexp-launcher/settings.
set -e
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
H=/home/cpi
# a copy of the small stock configuration files, just in case
B="$H/pspexp/stock-backup"
mkdir -p "$B"
for f in "$H/.bashrc" "$H/launchergo/.cpirc" "$H/launchergo/.xinitrc" "$H/launchergo/.xorg.conf" "$H/.mpd.conf"; do
	[ -f "$f" ] && cp -a "$f" "$B/" || true
done
chown -R cpi:cpi "$H/pspexp"

# the stock launcher's X session: never again at login, stopped now
if grep -q 'launchergo/.cpirc' "$H/.bashrc"; then
	sed -i '/^# PSPExp: the PSP console.s shell/d; /launchergo\/.cpirc/,/^fi$/d' "$H/.bashrc"
fi
# the X loop runs inside tty1's login shell: end that shell (autologin starts
# a new one, which no longer runs the block), then whatever it had started
pkill -KILL -t tty1 2>/dev/null || true
pkill -f '/home/cpi/launchergo' 2>/dev/null || true
pkill -x xinit 2>/dev/null || true
pkill -x Xorg 2>/dev/null || true
pkill -x aria2c 2>/dev/null || true
pkill -f gsnotify 2>/dev/null || true

# stock services and packages
systemctl disable --now shairport-sync 2>/dev/null || true
PKGS=""
for p in retroarch retroarch-assets gameshell-retroarch gameshell-love2d love mpd aria2 shairport-sync; do
	dpkg -s "$p" 2>/dev/null | grep -q '^Status: install ok installed' && PKGS="$PKGS $p"
done
[ -n "$PKGS" ] && DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=300 remove -y -q $PKGS
# libraries only they used; never the ones PSPExp and Chocolate Doom need
apt-mark manual libsdl2-2.0-0 chocolate-doom avahi-daemon >/dev/null 2>&1 || true
DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=300 autoremove -y -q

# stock content and the old launchers
rm -rf "$H/apps" "$H/games" "$H/launchergo" "$H/launcher" "$H/music" "$H/skins" "$H/.lima" "$H/.mpd.conf" "$H/.mpd"
rm -f /usr/local/bin/pspexp-tyrian
rm -rf /home/cpi/pspexp/opentyrian 2>/dev/null || true
# what the stock games, emulators and X session kept in the home folder
for d in .gnukem .hurrican .hydracastlelabyrinth .jfsw .lexaloffle .picodrive .powder .wicd .fehbg .twmrc \
	.chocolate-doom .lima.pspexp-off .config/GSPLauncher .config/mupen64plus .config/retroarch \
	.local/share/com.nesbox.tic .local/share/love .local/share/mupen64plus .local/share/chocolate-doom; do
	rm -rf "$H/$d"
done
# the launcher's state from the menu import; its settings stay
L="$H/.config/pspexp-launcher"
[ -d "$L" ] && find "$L" -mindepth 1 -maxdepth 1 ! -name settings -exec rm -rf {} +
echo "stock content removed; free: $(df -h / | awk 'NR==2{print $4}')"
