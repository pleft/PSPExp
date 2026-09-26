# Helpers for package recipes (sourced). $CACHE keeps downloads between builds.
# fetch URL [NAME]   -> prints the cached file's path (downloads it once)
fetch() {
	f="$CACHE/${2:-$(basename "$1")}"
	if [ ! -s "$f" ]; then
		curl -fsSL --retry 2 -o "$f.part" "$1" || { rm -f "$f.part"; echo "fetch failed: $1" >&2; return 1; }
		mv "$f.part" "$f"
	fi
	echo "$f"
}
# the Doom key map for the PSP (Chocolate Doom's defaults)
DOOM_KEYS="cross=lctrl,circle=space,square=lshift,triangle=return+y,l=comma,r=period,start=escape,select=tab,up=up,down=down,left=left,right=right"
