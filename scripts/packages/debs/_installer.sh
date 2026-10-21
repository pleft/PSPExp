# Installer packages for games PSPExp may not pass on (docs/Package_Manager.md
# §4.2): the package holds a launch script and nothing of the game; its
# postinst fetches the developer's own release from where the developer
# publishes it, checks a pinned SHA-256, unpacks it (unar: RAR and ZIP; tar for
# a .tar.gz, .tar.bz2 or .tar.xz, which unar only decompresses) into
# /usr/share/games/ID, and postrm removes it. Sourced by a game's build script,
# which sets:
#   ID TITLE VERSION       package name, the game's title, the package's version
#   SHA                    SHA-256 of the release archive
#   URL=...                a direct download, or
#   MIRRORS                more places for the same file, tried in turn when one fails or
#                          gives another file (the SHA-256 decides); the signed catalogue may
#                          add more ([mirrors] ID=URL lines in any recipe), so a dead site is a
#                          catalogue update, not a new package
#   FILE                   the release's file name (default: URL's); a copy placed by hand in
#                          /opt/pspexp/downloads/FILE is taken first (the last resort when every
#                          source is gone)
#   AFTER                  shell code run on the unpacked game before it takes its place
#                          ($G.new: Cave Story's data extracted from its program there)
#   DIR, CMD               the folder under /usr/share/games and the command in /usr/games
#                          (default: ID), where a recipe or a Debian program expects them
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
	DIR=${DIR:-$ID}; CMD=${CMD:-$ID}; FILE=${FILE:-$(basename "${URL%%\?*}")}
	FILE=$(printf '%b' "$(printf '%s' "$FILE" | sed 's/%\([0-9A-Fa-f][0-9A-Fa-f]\)/\\x\1/g')" | sed 's#.*/##')   # %2F, %20...: the name as saved
	case "$FILE" in *"'"*) echo "$ID: a FILE name with a quote" >&2; return 1 ;; esac   # (single-quoted in postinst)
	if [ "${PSPEXP_INSTALLER_LIST:-}" = 1 ]; then   # for scripts/packages/check-downloads: what it fetches
		echo "$ID $SHA ${URL:-itch:$ITCH} $MIRRORS"   # (no FILE: it may hold spaces)
		return 0
	fi
	_w=$(mktemp -d)
	mkdir -p "$_w/root/usr/games" "$_w/root/usr/share/doc/$ID" "$_w/ctl"
	cat > "$_w/root/usr/games/$CMD" <<S
#!/bin/sh
# $TITLE (package $ID: downloaded when it was installed)
G=/usr/share/games/$DIR
[ -f "\$G/.pspexp-sha256" ] || { echo "the game is not there: install the package $ID again (it downloads it)" >&2; exit 1; }
D="\${XDG_DATA_HOME:-\$HOME/.local/share}/$CMD"   # (the command's name: the folder a former package used, as for Quake)
mkdir -p "\$D"
$RUN
S
	chmod 755 "$_w/root/usr/games/$CMD"
	if [ -n "$ITCH" ]; then
		_u=${ITCH%%/*}; _r=${ITCH#*/}; _g=${_r%%/*}; _id=${_r#*/}
		_src="https://$_u.itch.io/$_g (upload $_id)"
		# a fresh link at each try: itch.io's last a minute, and now and then the link gave a small
		# web page instead of the game (twice in the 2026-10-21 audit, Theia and Celeste 64)
		_fetch="itchlink() { curl -fsSL -X POST 'https://$_u.itch.io/$_g/file/$_id?source=view_game&as_props=1&after_download_lightbox=true' | grep -o '\"url\":\"[^\"]*\"' | head -1 | cut -d'\"' -f4 | sed 's#\\\\/#/#g'; }
u=itch"
	else
		_src="$URL"
		_fetch="u='$URL'"
	fi
	# the sources in turn: the release's own, the mirrors, the catalogue's (the SHA-256 decides)
	_fetch="$_fetch
ok() { echo \"$SHA  \$C/game.part\" | sha256sum -c --quiet - >/dev/null 2>&1; }
got=
if [ -f '/opt/pspexp/downloads/$FILE' ] && cp '/opt/pspexp/downloads/$FILE' \"\$C/game.part\" && ok; then
	echo \"Using /opt/pspexp/downloads/$FILE\"; got=1
fi
if [ -z \"\$got\" ] && [ \"\$u\" = itch ]; then
	for n in 1 2 3; do
		v=\$(itchlink)
		[ -n \"\$v\" ] || { echo \"  itch.io gave no download link\"; sleep 5; continue; }
		echo \"Downloading $TITLE ($SIZE) from itch.io...\"
		if curl -fL --retry 2 --connect-timeout 20 -o \"\$C/game.part\" \"\$v\" && ok; then got=1; break; fi
		echo \"  not the expected file: asking itch.io again\"; sleep 5
	done
	u=
fi
if [ -z \"\$got\" ]; then
	for v in \${u:+\"\$u\"} $MIRRORS \$(awk -v id='$ID' '/^\\[/ { s = \$0; next } s == \"[mirrors]\" { i = index(\$0, \"=\"); k = substr(\$0, 1, i - 1); gsub(/[ \\t]/, \"\", k); if (i && k == id) { v = substr(\$0, i + 1); gsub(/^[ \\t]+|[ \\t]+\$/, \"\", v); print v } }' /usr/share/pspexp/catalogue/recipes/*/recipe 2>/dev/null); do
		echo \"Downloading $TITLE ($SIZE) from \$(echo \"\$v\" | cut -d/ -f3)...\"
		if curl -fL --retry 2 --connect-timeout 20 -o \"\$C/game.part\" \"\$v\" && ok; then got=1; break; fi
		echo \"  not there, or not the expected file: the next source\"
	done
fi
[ -n \"\$got\" ] || { rm -rf \"\$C\"; echo \"no source gave the expected file: install it again later, or put '$FILE' in /opt/pspexp/downloads and install again\" >&2; exit 1; }"
	case "$URL" in
	*.tar.gz|*.tgz) _unpack='tar -xzf "$C/game.part" -C "$C/x"' ;;   # (unar knows a compressed tar by its name only:
	*.tar.bz2|*.tbz2|*.tbz) _unpack='tar -xjf "$C/game.part" -C "$C/x"' ;;   #  on game.part it just decompresses it)
	*.tar.xz|*.txz) _unpack='tar -xJf "$C/game.part" -C "$C/x"' ;;
	*) _unpack='unar -q -D -o "$C/x" "$C/game.part" >/dev/null' ;;   # -D: no folder of its own round an archive of several top-level items (named after game.part)
	esac
	[ -n "$KEEP" ] && { _unpack="mkdir -p \"\$C/x/g\" && mv \"\$C/game.part\" \"\$C/x/g/$KEEP\""; TOP=g; }
	cat > "$_w/root/usr/share/doc/$ID/README" <<D
$TITLE. This package holds no part of the game: installing it downloads the
developer's release from
  $_src
(or the same file from a mirror${MIRRORS:+: $MIRRORS}; or a copy placed in
/opt/pspexp/downloads/$FILE), checks its SHA-256 ($SHA) and unpacks it into
/usr/share/games/$DIR, every
file as released (the developer's own notes and terms are among them).
Removing the package removes it. Installer by PSPExp.
D
	cat > "$_w/ctl/postinst" <<P
#!/bin/sh
# fetch, check and unpack $TITLE (the package carries none of it)
set -e
[ "\$1" = configure ] || exit 0
G=/usr/share/games/$DIR
C=/var/cache/$ID
[ -f "\$G/.pspexp-sha256" ] && [ "\$(cat "\$G/.pspexp-sha256")" = "$SHA" ] && exit 0
mkdir -p "\$C"
$_fetch
echo "Unpacking..."
mkdir -p "\$C/x"
$_unpack
mkdir -p /usr/share/games   # (not there on a minimal system: a fresh card, or no other game yet)
rm -rf "\$G.new"; mkdir -p "\$(dirname "\$G")"; mv "\$C/x/$TOP" "\$G.new"   # (a DIR inside another package's folder: commandergenius/games/keen1)
$AFTER
chmod -R u=rwX,go=rX "\$G.new"
echo "$SHA" > "\$G.new/.pspexp-sha256"
rm -rf "\$G"; mv "\$G.new" "\$G"
rm -rf "\$C"
P
	cat > "$_w/ctl/postrm" <<R
#!/bin/sh
# the game the installer unpacked goes with the package
set -e
case "\$1" in remove|purge) rm -rf /usr/share/games/$DIR /usr/share/games/$DIR.new /var/cache/$ID ;; esac
exit 0
R
	cat > "$_w/ctl/control" <<C
Package: $ID
Version: $VERSION
Architecture: all
Maintainer: PSPExp <info@pspexp.dev>
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
