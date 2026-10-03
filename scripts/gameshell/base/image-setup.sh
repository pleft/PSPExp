#!/bin/sh
# Runs inside the image build container (build-image.sh): /out/in holds the
# inputs, /out/kernel the kernel, /work is scratch. Builds the Debian 13 armhf
# root file system (as small as the board allows), configures it for the
# GameShell, installs PSPExp from its apt repository (pspexp-core; games and
# emulators are for the user to add through Apps > Packages), applies the
# GameShell's own files, and assembles /out/pspexp-gameshell.img.
set -e
export DEBIAN_FRONTEND=noninteractive
IN=/out/in K=/out/kernel W=/work
apt-get -qq update && apt-get -qq install -y --no-install-recommends \
	mmdebstrap arch-test debian-archive-keyring dosfstools mtools e2fsprogs fdisk u-boot-tools ca-certificates >/dev/null
R=$W/rootfs
rm -rf "$R"
# only what the board and PSPExp need: Wi-Fi (NetworkManager; hostapd for the
# PSP's own access point), the USB network (ifupdown, dhclient), SSH and .local
# names, Bluetooth (bluez; PipeWire and its Bluetooth plugin for headphones, pspexp-bt), the
# clock, growing the card at first boot, a small editor for the terminal, and ffmpeg (the music
# player's formats beyond MP3 and WAV); PSPExp itself and
# its dependencies come from apt below
PKGS="systemd-sysv,udev,dbus,openssh-server,sudo,network-manager,wpasupplicant,hostapd,iw,wireless-regdb,\
firmware-brcm80211,curl,ca-certificates,gpgv,avahi-daemon,libnss-mdns,ifupdown,isc-dhcp-client,\
cloud-guest-utils,fdisk,iproute2,kmod,nano,less,procps,tzdata,systemd-timesyncd,locales,bluez,\
pipewire,pipewire-bin,wireplumber,libspa-0.2-bluetooth,rfkill,ffmpeg"
mmdebstrap --arch=armhf --variant=minbase --mode=root --include="$PKGS" \
	--components="main contrib non-free-firmware" trixie "$R" \
	"deb http://deb.debian.org/debian trixie main contrib non-free-firmware" \
	"deb http://deb.debian.org/debian trixie-updates main contrib non-free-firmware" \
	"deb http://security.debian.org/debian-security trixie-security main contrib non-free-firmware"

# ---- the board ------------------------------------------------------------------
echo gameshell > "$R/etc/hostname"
printf '127.0.0.1\tlocalhost\n127.0.1.1\tgameshell\n::1\tlocalhost ip6-localhost ip6-loopback\n' > "$R/etc/hosts"
DISKID=0x5053504d   # "PSPM": the partitions' PARTUUIDs are 5053504d-01 and -02
cat > "$R/etc/fstab" <<F
PARTUUID=5053504d-02  /      ext4  defaults,noatime  0 1
PARTUUID=5053504d-01  /boot  vfat  defaults,umask=022  0 2
F
mkdir -p "$R/boot"
# kernel modules; the USB network gadget and the virtual keypad at boot
tar -C "$R/usr" -xzf "$K/modules.tar.gz"
KR=$(cat "$K/release")
chroot "$R" depmod -a "$KR"
printf 'g_ether\nuinput\n' > "$R/etc/modules-load.d/pspexp-base.conf"   # (pspexp.conf is the stage's old name, removed by it)
# fixed addresses: the computer sees the same USB network device every time
echo 'options g_ether host_addr=02:50:53:50:00:01 dev_addr=02:50:53:50:00:02' > "$R/etc/modprobe.d/g_ether.conf"
# the Wi-Fi chip's settings for this board (Ampak's AP6212 NVRAM, in scripts/gameshell/base)
# (the firmware is Debian's, firmware-brcm80211; brcmfmac looks for the board's
# settings file by the device tree's compatible)
mkdir -p "$R/usr/lib/firmware/updates/brcm"
cp "$IN/fw/brcmfmac43430a0-sdio.clockwork,clockworkpi-cpi3.txt" "$R/usr/lib/firmware/updates/brcm/"
# its Bluetooth firmware patch (ClockworkPi's, fetched by build-image.sh; hci_bcm asks for this name)
cp "$IN/fw/BCM4343A0.hcd" "$R/usr/lib/firmware/updates/brcm/"
# locale
sed -i 's/^# *en_US.UTF-8/en_US.UTF-8/' "$R/etc/locale.gen"
chroot "$R" locale-gen >/dev/null
echo 'LANG=en_US.UTF-8' > "$R/etc/default/locale"

# ---- the cpi user ----------------------------------------------------------------
chroot "$R" useradd -m -u 1000 -s /bin/bash -G sudo,audio,video,plugdev,netdev,input,dialout cpi
echo 'cpi:cpi' | chroot "$R" chpasswd
echo 'cpi ALL=(ALL) NOPASSWD: ALL' > "$R/etc/sudoers.d/cpi"; chmod 440 "$R/etc/sudoers.d/cpi"
# cpi's PipeWire runs without a login (as loginctl enable-linger would): the console is a
# system service, and Bluetooth sound goes through the user's PipeWire (pspexp-bt)
mkdir -p "$R/var/lib/systemd/linger" && touch "$R/var/lib/systemd/linger/cpi"
if [ -f "$IN/authorized_keys" ]; then
	install -d -m 700 -o 1000 -g 1000 "$R/home/cpi/.ssh"
	install -m 600 -o 1000 -g 1000 "$IN/authorized_keys" "$R/home/cpi/.ssh/authorized_keys"
fi
for c in "$IN"/wifi/*.nmconnection; do
	[ -f "$c" ] && install -m 600 "$c" "$R/etc/NetworkManager/system-connections/"
done

# ---- PSPExp from its apt repository (docs/Package_Manager.md §8) ---------------------
# the board set up for the repository (kind, key, source, root helper, sudo rule,
# the signed catalogue), then pspexp-core and what it needs, as on a running
# GameShell (scripts/gameshell/install.sh)
cp /etc/resolv.conf "$R/etc/resolv.conf"
cp "$R/etc/hosts" $W/hosts.card; cp /etc/hosts "$R/etc/hosts"   # the build's (a .local repository's address), put back below
mount -t proc proc "$R/proc"
cp "$IN/pkg/keyring.gpg" "$IN/pkg/pspexp-apt-setup" "$IN/pkg/pspexp-pkg-apt" "$R/tmp/"
chroot "$R" sh /tmp/pspexp-apt-setup gameshell cpi "$(cat "$IN/pkg/repo-url")" /tmp/keyring.gpg /tmp/pspexp-pkg-apt
chroot "$R" apt-get -qq install -y --no-install-recommends pspexp-core >/dev/null
chroot "$R" dpkg-query -W pspexp-core pspexp-runtime
rm -f "$R"/tmp/keyring.gpg "$R"/tmp/pspexp-apt-setup "$R"/tmp/pspexp-pkg-apt
umount "$R/proc"
rm -f "$R/etc/resolv.conf"; cp $W/hosts.card "$R/etc/hosts"

# ---- PSPExp: the GameShell's own files (the staged list) --------------------------
while read -r kind a b c d; do
	case $kind in
	dir)  install -d -m "$a" -o "$b" -g "$c" "$R$d" ;;
	file) mkdir -p "$R$(dirname "$d")"; install -m "$a" -o "$b" -g "$c" "$IN/stage/root$d" "$R$d" ;;
	link) mkdir -p "$R$(dirname "$b")"; ln -sfn "$a" "$R$b" ;;
	rm)   rm -f "$R$a" ;;
	esac
done < "$IN/stage/manifest"
chroot "$R" systemctl enable ssh NetworkManager avahi-daemon systemd-timesyncd >/dev/null 2>&1 || true
# pspexp-wifi-mode runs hostapd itself, with its own configuration
chroot "$R" systemctl disable hostapd >/dev/null 2>&1 || true
chroot "$R" systemctl mask hostapd >/dev/null 2>&1 || true
chroot "$R" apt-get clean
rm -rf "$R"/var/lib/apt/lists/* "$R"/etc/ssh/ssh_host_*   # host keys: made on first boot
# regenerate SSH host keys on first boot (Debian does it when missing)
cat > "$R/etc/systemd/system/ssh-hostkeys.service" <<S
[Unit]
Description=SSH host keys for this card
Before=ssh.service
ConditionPathExists=!/etc/ssh/ssh_host_ed25519_key
[Service]
Type=oneshot
ExecStart=/usr/bin/ssh-keygen -A
[Install]
WantedBy=multi-user.target
S
chroot "$R" systemctl enable ssh-hostkeys >/dev/null 2>&1
# Linux's side of the breadcrumbs: its log and network state on the boot
# partition a minute after each boot, and a journal kept across boots
mkdir -p "$R/var/log/journal"
cat > "$R/usr/local/sbin/pspexp-bootlog" <<'L'
#!/bin/sh
sleep 60
{ echo "== $(date) $(uname -a)"; echo "== failed units"; systemctl --failed --no-legend --no-pager
  echo "== addresses"; ip -br addr; echo "== usb"; ls /sys/class/udc 2>/dev/null; cat /sys/class/udc/*/state 2>/dev/null
  echo "== dmesg"; dmesg; } > /boot/pspexp-boot-log.txt 2>&1
sync
L
chmod 755 "$R/usr/local/sbin/pspexp-bootlog"
cat > "$R/etc/systemd/system/pspexp-bootlog.service" <<S
[Unit]
Description=PSPExp boot log on the boot partition (for cards without a serial console)
After=boot.mount
[Service]
# in the background: it waits a minute, and must not hold the boot (the
# console starts after multi-user.target)
Type=simple
ExecStart=/usr/local/sbin/pspexp-bootlog
[Install]
WantedBy=multi-user.target
S
chroot "$R" systemctl enable pspexp-bootlog >/dev/null 2>&1

# ---- boot partition: the kernel, the device tree, the boot script -------------------
# breadcrumbs on the boot partition (no serial console: put the card in a
# computer to see how far a failed boot got): U-Boot writes pspexp-1-script,
# pspexp-2-loaded, and pspexp-3-bootm-failed if the kernel was refused
cat > $W/boot.cmd <<'B'
gpio clear PB7
setenv bootargs console=ttyS0,115200n8 root=PARTUUID=5053504d-02 rootfstype=ext4 rootwait panic=10 loglevel=7
mw.b 0x4a000000 0x2e 1
fatwrite mmc 0:1 0x4a000000 pspexp-1-script 1
fatload mmc 0 0x48000000 uImage
fatload mmc 0 0x49000000 sun8i-r16-clockworkpi-cpi3.dtb
fatwrite mmc 0:1 0x4a000000 pspexp-2-loaded 1
bootm 0x48000000 - 0x49000000
fatwrite mmc 0:1 0x4a000000 pspexp-3-bootm-failed 1
B
mkimage -A arm -O linux -T script -C none -d $W/boot.cmd $W/boot.scr >/dev/null
BOOT=$W/boot.img; rm -f $BOOT
mkfs.vfat -C -n BOOT $BOOT 65536 >/dev/null
mcopy -i $BOOT "$K/uImage" "$K/sun8i-r16-clockworkpi-cpi3.dtb" $W/boot.scr $W/boot.cmd ::/
cp "$K/uImage" "$K/sun8i-r16-clockworkpi-cpi3.dtb" "$R/boot/" 2>/dev/null || true

# ---- the root file system and the card image ------------------------------------------
ROOTIMG=$W/root.img; rm -f $ROOTIMG
USED=$(du -sm "$R" | cut -f1)
SIZE=$(( USED + USED / 3 + 400 ))   # MiB; the first boot grows it to the whole card
mkfs.ext4 -q -L rootfs -d "$R" $ROOTIMG ${SIZE}M
IMG=/out/pspexp-gameshell.img; rm -f $IMG
TOTAL=$(( 4 + 64 + SIZE ))
truncate -s ${TOTAL}M $IMG
dd if="$IN/uboot.bin" of=$IMG bs=512 seek=16 conv=notrunc status=none   # at 8 KiB, where the SoC looks
sfdisk -q $IMG <<P
label: dos
label-id: $DISKID
start=8192, size=131072, type=c, bootable
start=139264, type=83
P
dd if=$BOOT of=$IMG bs=512 seek=8192 conv=notrunc status=none
dd if=$ROOTIMG of=$IMG bs=512 seek=139264 conv=notrunc status=none
echo "image: $TOTAL MiB, root $SIZE MiB (used $USED MiB), kernel $KR"
