#!/bin/sh
# Runs inside the image build container (build-image.sh): /out/cache/base.img.xz is
# Raspberry Pi OS Lite, /out/in the inputs, /work scratch. Grows its root file
# system, installs PSPExp from its apt repository in a chroot (natively: the
# container is arm64 like the Pi), sets up the pi user and the Pi's own files, and
# writes /out/pspexp-pi.img. The boot partition, the kernel command line and the
# disk's identifier stay as they are: Raspberry Pi OS's first boot (resizing the
# card, SSH host keys, cloud-init for the Imager's settings) runs as on its own image.
set -e
export DEBIAN_FRONTEND=noninteractive
IN=/out/in W=/work
apt-get -qq update && apt-get -qq install -y --no-install-recommends xz-utils fdisk e2fsprogs ca-certificates >/dev/null
IMG=$W/pi.img
xz -dc /out/cache/base.img.xz > $IMG
# room for PSPExp and what it needs; the first boot grows it to the whole card
truncate -s +1536M $IMG
echo ', +' | sfdisk -q -N 2 $IMG
B0=$(sfdisk -d $IMG | awk -F'[=,]' '/img1 / {print $2+0}'); BN=$(sfdisk -d $IMG | awk -F'[=,]' '/img1 / {print $3+0}')
R0=$(sfdisk -d $IMG | awk -F'[=,]' '/img2 / {print $2+0}'); RN=$(sfdisk -d $IMG | awk -F'[=,]' '/img2 / {print $3+0}')
RL=$(losetup -f --show -o $((R0 * 512)) --sizelimit $((RN * 512)) $IMG)
BL=$(losetup -f --show -o $((B0 * 512)) --sizelimit $((BN * 512)) $IMG)
cleanup() {
	for m in dev/pts dev sys proc boot/firmware ""; do umount "$R/$m" 2>/dev/null || true; done
	losetup -d "$RL" "$BL" 2>/dev/null || true
}
trap cleanup EXIT
e2fsck -fy $RL >/dev/null 2>&1 || true
resize2fs $RL >/dev/null 2>&1
R=$W/root; mkdir -p $R
mount $RL $R; mount $BL $R/boot/firmware
mount -t proc proc $R/proc; mount -t sysfs sys $R/sys; mount --bind /dev $R/dev; mount --bind /dev/pts $R/dev/pts
# the build's name server for apt (the card's resolv.conf, a file or a link, put back below)
rm -rf $W/resolv.card; mv $R/etc/resolv.conf $W/resolv.card; cp -L /etc/resolv.conf $R/etc/resolv.conf
cp $R/etc/hosts $W/hosts.card; cp /etc/hosts $R/etc/hosts   # the build's (a .local repository's address), put back below
# no service starts inside the chroot
printf '#!/bin/sh\nexit 101\n' > $R/usr/sbin/policy-rc.d; chmod 755 $R/usr/sbin/policy-rc.d

# ---- the pi user ---------------------------------------------------------------------
# Raspberry Pi OS ships pi as a placeholder (no login) for its first-boot dialog to
# rename; here pi is the user, so the dialog (which asks on the Pi's own screen) is off.
# The Imager's settings, if any, still come through cloud-init.
chroot $R usermod -s /bin/bash pi
echo 'pi:pspexp' | chroot $R chpasswd
echo 'pi ALL=(ALL) NOPASSWD: ALL' > $R/etc/sudoers.d/010_pi-nopasswd; chmod 440 $R/etc/sudoers.d/010_pi-nopasswd
chroot $R systemctl disable userconfig >/dev/null 2>&1
if [ -f $IN/authorized_keys ]; then
	install -d -m 700 -o 1000 -g 1000 $R/home/pi/.ssh
	install -m 600 -o 1000 -g 1000 $IN/authorized_keys $R/home/pi/.ssh/authorized_keys
	chroot $R systemctl enable ssh >/dev/null 2>&1
fi

# ---- PSPExp from its apt repository (docs/Package_Manager.md §8) -------------------------
# the board set up for the repository, then pspexp-core, and what a Desktop Raspberry Pi
# OS has and Lite has not: PipeWire and its Bluetooth plugin (headphones, pspexp-bt) and
# ffmpeg (the music player's formats beyond MP3 and WAV)
mkdir -p $R/tmp/pkg; cp $IN/pkg/* $IN/board-files.sh $R/tmp/pkg/
chroot $R apt-get -qq update
chroot $R sh /tmp/pkg/pspexp-apt-setup pi pi "$(cat $IN/pkg/repo-url)" /tmp/pkg/keyring.gpg /tmp/pkg/pspexp-pkg-apt
chroot $R apt-get -qq install -y --no-install-recommends pspexp-core \
	pipewire pipewire-bin wireplumber libspa-0.2-bluetooth ffmpeg >/dev/null
chroot $R dpkg-query -W pspexp-core pspexp-runtime
chroot $R sh /tmp/pkg/board-files.sh pi /usr/bin/pspexp-term

# Wi-Fi: Raspberry Pi OS keeps it blocked until a country is set (raspi-config, or the
# Imager); joined from the PSP (Settings), it must be on: unblocked at boot when no
# country was given, on the worldwide rules, which hold everywhere
cat > $R/etc/systemd/system/pspexp-wifi.service <<'S'
[Unit]
Description=PSPExp: Wi-Fi on without a country set (the worldwide rules)
After=systemd-rfkill.service
Before=NetworkManager.service
[Service]
Type=oneshot
ExecStart=/bin/sh -c 'grep -q cfg80211.ieee80211_regdom= /proc/cmdline || /usr/sbin/rfkill unblock wifi'
[Install]
WantedBy=multi-user.target
S
chroot $R systemctl enable pspexp-wifi >/dev/null 2>&1

# ---- tidy up -----------------------------------------------------------------------------
rm -rf $R/tmp/pkg $R/usr/sbin/policy-rc.d
chroot $R apt-get clean
rm -rf $R/var/lib/apt/lists/*
rm -f $R/etc/resolv.conf; mv $W/resolv.card $R/etc/resolv.conf
cp $W/hosts.card $R/etc/hosts
ls $R/etc/ssh/ssh_host_* >/dev/null 2>&1 && { echo "SSH host keys in the image: removed (made on first boot)"; rm -f $R/etc/ssh/ssh_host_*; }
USED=$(df -m $R | awk 'NR==2 {print $3}')
# as small as it holds (a quicker card to write), with room for the first boot; it then grows to the card
for p in /proc/[0-9]*; do   # what apt left running in the chroot (gpg-agent and the like)
	[ "$(readlink $p/root 2>/dev/null)" = "$R" ] && { echo "stopping $(cat $p/comm) left in the chroot"; kill ${p#/proc/}; }
done
sleep 1
for m in dev/pts dev sys proc boot/firmware ""; do umount "$R/$m" || { echo "cannot unmount $R/$m" >&2; exit 1; }; done
e2fsck -fy $RL >/dev/null 2>&1 || true
MIN=$(resize2fs -P $RL 2>/dev/null | awk -F': ' '/minimum/ {print $2}')   # 4 KiB blocks, metadata included
NEW=$(( MIN / 256 + 256 ))   # MiB
resize2fs $RL ${NEW}M >/dev/null
cleanup; trap - EXIT
echo "size=$(( NEW * 2048 ))" | sfdisk -q -N 2 $IMG
truncate -s $(( (R0 + NEW * 2048) * 512 )) $IMG
cp $IMG /out/pspexp-pi.img
echo "image: $(( $(stat -c %s /out/pspexp-pi.img) / 1048576 )) MiB, root used $USED MiB"
