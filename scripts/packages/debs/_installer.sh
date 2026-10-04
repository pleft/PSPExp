# Installer packages for games PSPExp may not pass on (docs/Package_Manager.md
# §4.2): the package holds a launch script and nothing of the game; its
# postinst fetches the developer's own release from where the developer
# publishes it, checks a pinned SHA-256, unpacks it (unar: RAR and ZIP; tar for
# a .tar.gz, which unar only ungzips) into
# /usr/share/games/ID, and postrm removes it. Sourced by a game's build script,
# which sets:
#   ID TITLE VERSION       package name, the game's title, the package's version
#   SHA                    SHA-256 of the release archive
#   URL=...                a direct download, or
#   ITCH=user/game/upload  an itch.io upload (a free game's link is asked for at
#                          each install: itch.io's links last a minute)
#   SIZE                   the download's size, for the message ("575 MB")
#   TOP                    the folder inside the archive that holds the game ("" when at the top)
#   KEEP                   instead of TOP: the download is kept whole under this name (a
#                          Solarus quest's .solarus file, which the engine reads as it is)
#   RUN                    the launch command, $G being the game's folder (shell code)
#   DEPENDS                packages the game needs besides curl and unar
#   ABOUT                  the package description's lines (one space-indented each)
# then calls build_installer OUTDIR.

build_installer() {
	_out=$1
	_root="$(cd "$(dirname "$0")/../../../.." && pwd)"
	_w=$(mktemp -d)
	mkdir -p "$_w/root/usr/games" "$_w/root/usr/share/doc/$ID" "$_w/ctl"
	cat > "$_w/root/usr/games/$ID" <<S
#!/bin/sh
# $TITLE (package $ID: downloaded when it was installed)
G=/usr/share/games/$ID
[ -f "\$G/.pspexp-sha256" ] || { echo "the game is not there: install the package $ID again (it downloads it)" >&2; exit 1; }
D="\${XDG_DATA_HOME:-\$HOME/.local/share}/$ID"
mkdir -p "\$D"
$RUN
S
	chmod 755 "$_w/root/usr/games/$ID"
	if [ -n "$ITCH" ]; then
		_u=${ITCH%%/*}; _r=${ITCH#*/}; _g=${_r%%/*}; _id=${_r#*/}
		_src="https://$_u.itch.io/$_g (upload $_id)"
		_fetch="u=\$(curl -fsSL -X POST 'https://$_u.itch.io/$_g/file/$_id?source=view_game&as_props=1&after_download_lightbox=true' | grep -o '\"url\":\"[^\"]*\"' | head -1 | cut -d'\"' -f4 | sed 's#\\\\/#/#g')
[ -n \"\$u\" ] || { echo \"itch.io gave no download link\" >&2; exit 1; }"
	else
		_src="$URL"
		_fetch="u='$URL'"
	fi
	case "$URL" in
	*.tar.gz|*.tgz) _unpack='tar -xzf "$C/game.part" -C "$C/x"' ;;
	*) _unpack='unar -q -D -o "$C/x" "$C/game.part" >/dev/null' ;;   # -D: no folder of its own round an archive of several top-level items (named after game.part)
	esac
	[ -n "$KEEP" ] && { _unpack="mkdir -p \"\$C/x/g\" && mv \"\$C/game.part\" \"\$C/x/g/$KEEP\""; TOP=g; }
	cat > "$_w/root/usr/share/doc/$ID/README" <<D
$TITLE. This package holds no part of the game: installing it downloads the
developer's release from
  $_src
checks its SHA-256 ($SHA) and unpacks it into /usr/share/games/$ID, every
file as released (the developer's own notes and terms are among them).
Removing the package removes it. Installer by PSPExp.
D
	cat > "$_w/ctl/postinst" <<P
#!/bin/sh
# fetch, check and unpack $TITLE (the package carries none of it)
set -e
[ "\$1" = configure ] || exit 0
G=/usr/share/games/$ID
C=/var/cache/$ID
[ -f "\$G/.pspexp-sha256" ] && [ "\$(cat "\$G/.pspexp-sha256")" = "$SHA" ] && exit 0
mkdir -p "\$C"
echo "Downloading $TITLE ($SIZE) from its developer's page..."
$_fetch
curl -fL --retry 3 --connect-timeout 20 -o "\$C/game.part" "\$u" || { rm -rf "\$C"; echo "the download failed: install it again" >&2; exit 1; }
echo "$SHA  \$C/game.part" | sha256sum -c --quiet - || { rm -rf "\$C"; echo "the download is not the expected file (a new release?)" >&2; exit 1; }
echo "Unpacking..."
mkdir -p "\$C/x"
$_unpack
rm -rf "\$G.new"; mv "\$C/x/$TOP" "\$G.new"
chmod -R u=rwX,go=rX "\$G.new"
echo "$SHA" > "\$G.new/.pspexp-sha256"
rm -rf "\$G"; mv "\$G.new" "\$G"
rm -rf "\$C"
P
	cat > "$_w/ctl/postrm" <<R
#!/bin/sh
# the game the installer unpacked goes with the package
set -e
case "\$1" in remove|purge) rm -rf /usr/share/games/$ID /usr/share/games/$ID.new /var/cache/$ID ;; esac
exit 0
R
	cat > "$_w/ctl/control" <<C
Package: $ID
Version: $VERSION
Architecture: all
Maintainer: PSPExp <pspexp@localhost>
Section: contrib/games
Priority: optional
Depends: curl, ca-certificates, unar${DEPENDS:+, $DEPENDS}${CONFLICTS:+
Conflicts: $CONFLICTS
Replaces: $CONFLICTS}
Description: $TITLE (installer)
$ABOUT
 Installing this package downloads the game's own release and checks it;
 the package itself holds none of the game. Installer by PSPExp.
C
	python3 "$_root/scripts/packages/mkdeb.py" "$_w/root" "$_w/ctl/control" "$_out/${ID}_${VERSION}_all.deb"
	rm -rf "$_w"
}
