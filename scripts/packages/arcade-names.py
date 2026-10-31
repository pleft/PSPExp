#!/usr/bin/env python3
"""
An arcade system's names table for the launcher (a [system]'s names=): one line per set,
"set|Full title", read from FinalBurn Neo's driver sources. Arcade games are zips named after
their set ("mslug.zip"); the launcher shows "Metal Slug - Super Vehicle-001" instead. A set
that is no game (the Neo Geo's BIOS, neogeo.zip: a board ROM) gets an empty title: the
launcher leaves it out of the list.

  scripts/packages/arcade-names.py FBNEO_SOURCE DRIVER_DIR OUT
  e.g. scripts/packages/arcade-names.py ../retro/libretro-fbneo neogeo build/neogeo.names

Only reads the sources (the BurnDriver tables in src/burn/drv/DRIVER_DIR/*.cpp).
"""
import glob, os, re, sys

DRIVER = re.compile(r'struct\s+BurnDriver\w*\s+\w+\s*=\s*\{\s*"([^"]+)"[^\n]*\n\s*"((?:[^"\\]|\\.)*)"[^\n]*\n\s*[^\n]*\n\s*([^\n]*)', re.M)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__.strip())
    src, drv, out = sys.argv[1:]
    files = sorted(glob.glob(os.path.join(src, "src", "burn", "drv", drv, "*.cpp")))
    if not files:
        sys.exit("no driver sources in %s" % os.path.join(src, "src", "burn", "drv", drv))
    names = {}
    for f in files:
        with open(f, encoding="latin-1") as fh:
            text = fh.read()
        for m in DRIVER.finditer(text):
            name, title, flags = m.group(1), m.group(2), m.group(3)
            title = title.split("\\0")[0].replace('\\"', '"').strip()
            if " / " in title:   # (alternative names: the first, "Samurai Shodown / Samurai Spirits": readable on the PSP)
                title = title.split(" / ")[0].strip()
            if "BDF_BOARDROM" in flags:
                title = ""   # (a BIOS: not a game)
            names.setdefault(name, title)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w", encoding="utf-8") as fh:
        fh.write("# set|title (an empty title: no game, left out of the list); from FinalBurn Neo's drivers\n")
        for k in sorted(names):
            fh.write("%s|%s\n" % (k, names[k]))
    print("%s: %d sets (%d not games)" % (out, len(names), sum(1 for v in names.values() if not v)))


if __name__ == "__main__":
    main()
