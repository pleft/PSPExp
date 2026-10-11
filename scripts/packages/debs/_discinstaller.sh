# Installer packages for games EA released as freeware (Command & Conquer,
# Red Alert): EA's own freeware release, as archived from its download page
# (archive.org, "Command & Conquer Classics Original Freeware Releases
# 2007-2010"), is downloaded at install, checked against a pinned SHA-256, the
# disc image taken out of it when it is an archive, and the game's MIX files
# out of the disc image (7z) into /usr/share/games/ID; postrm removes them.
# Nothing of the games is hosted by PSPExp. Sourced by a game's build script,
# which sets ID TITLE VERSION URL SHA SIZE PROGRAM (the Vanilla Conquer
# program) ABOUT, then calls build_discinstaller OUTDIR.
build_discinstaller() {
	_out=$1
	_root="$(cd "$(dirname "$0")/../../../.." && pwd)"
	_w=$(mktemp -d)
	mkdir -p "$_w/root/usr/games" "$_w/root/usr/share/doc/$ID" "$_w/ctl"
	cat > "$_w/root/usr/games/$ID" <<S
#!/bin/sh
# $TITLE (package $ID: EA's freeware, downloaded when it was installed), on Vanilla Conquer
G=/usr/share/games/$ID
[ -f "\$G/.pspexp-sha256" ] || { echo "the game is not there: install the package $ID again (it downloads it)" >&2; exit 1; }
cd "\$G" && exec /usr/lib/pspexp/vanillaconquer/$PROGRAM "\$@"
S
	chmod 755 "$_w/root/usr/games/$ID"
	cat > "$_w/root/usr/share/doc/$ID/README" <<D
$TITLE, EA's freeware release. This package holds none of the game:
installing it downloads EA's release as archived from EA's page
  $URL
checks its SHA-256 ($SHA) and puts the game's MIX files from its disc image
in /usr/share/games/$ID, for Vanilla Conquer (package pspexp-vanillaconquer).
EA's licence agreement, on the disc, applies. Removing the package removes them.
D
	cat > "$_w/ctl/postinst" <<P
#!/bin/sh
# fetch $TITLE's freeware disc, check it, take its game files out
set -e
[ "\$1" = configure ] || exit 0
G=/usr/share/games/$ID
C=/var/cache/$ID
[ -f "\$G/.pspexp-sha256" ] && [ "\$(cat "\$G/.pspexp-sha256")" = "$SHA" ] && exit 0
rm -rf "\$C"; mkdir -p "\$C"
echo "Downloading $TITLE ($SIZE), EA's freeware release..."
curl -fL --retry 3 --connect-timeout 20 -o "\$C/game.part" "$URL" || { rm -rf "\$C"; echo "the download failed: install it again" >&2; exit 1; }
echo "$SHA  \$C/game.part" | sha256sum -c --quiet - || { rm -rf "\$C"; echo "the download is not the expected file" >&2; exit 1; }
echo "Unpacking..."
I="\$C/game.part"
case "$URL" in *.rar|*.zip) mkdir -p "\$C/x"; unar -q -D -o "\$C/x" "\$C/game.part" >/dev/null; rm -f "\$C/game.part"; I=\$(find "\$C/x" -iname '*.iso' | head -1) ;; esac
[ -n "\$I" ] && [ -f "\$I" ] || { rm -rf "\$C"; echo "no disc image in the download" >&2; exit 1; }
rm -rf "\$G.new"; mkdir -p "\$G.new"
7z e -y -r -o"\$G.new" "\$I" '*.MIX' >/dev/null
ls "\$G.new"/*.MIX >/dev/null 2>&1 || { rm -rf "\$G.new" "\$C"; echo "no game files on the disc" >&2; exit 1; }
chmod -R u=rwX,go=rX "\$G.new"
echo "$SHA" > "\$G.new/.pspexp-sha256"
rm -rf "\$G"; mv "\$G.new" "\$G"; rm -rf "\$C"
P
	cat > "$_w/ctl/postrm" <<R
#!/bin/sh
set -e
case "\$1" in remove|purge) rm -rf /usr/share/games/$ID /usr/share/games/$ID.new /var/cache/$ID ;; esac
exit 0
R
	cat > "$_w/ctl/control" <<C
Package: $ID
Version: $VERSION
Architecture: all
Maintainer: PSPExp <info@pspexp.dev>
Section: contrib/games
Priority: optional
Depends: curl, ca-certificates, unar, 7zip, pspexp-vanillaconquer
Description: $TITLE (installer)
$ABOUT
 Installing this package downloads EA's freeware release and checks it;
 the package itself holds none of the game. Installer by PSPExp.
C
	python3 "$_root/scripts/packages/mkdeb.py" "$_w/root" "$_w/ctl/control" "$_out/${ID}_${VERSION}_all.deb"
	rm -rf "$_w"
}
