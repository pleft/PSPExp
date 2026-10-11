#!/usr/bin/env python3
"""
The package manager's failure paths on a Debian board (docs/Package_Manager.md §10.3).
  tests/pkg/run.py --host user@board
Builds signed test catalogues (the real recipes plus throwaway pkgtest-*
ones; a tampered copy; an older one) with the project key, copies them and
tests/pkg/failures.sh to the board, and runs it. The board's real catalogue is
put back at the end.
"""
import os, shutil, subprocess, sys, tarfile, tempfile, time, io

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GNUPG = os.path.expanduser("~/.pspexp/gnupg")
KEY = "PSPExp archive <pspexp@localhost>"   # the signing key's identity

TEST_RECIPES = {
"pkgtest-hello": """[package]
id=pkgtest-hello
title=Package test: hello
description=A throwaway recipe for the package manager's tests
category=app
license=GPL-3.0 (GNU hello)
version=1

[debian]
apt=hello

[entry]
title=hello
type=app
exec=/usr/bin/hello
native=1
""",
"pkgtest-sys": """[package]
id=pkgtest-sys
title=Package test: a system
description=A throwaway system recipe for the package manager's tests
category=system
license=GPL-2.0 (Nestopia)
version=1

[debian]
apt=libretro-nestopia pspexp-retro
core=nestopia_libretro.so

[system]
id=pkgtest-sys
name=Package test system
short=PKGT
ext=pkgt
roms=~/roms/pkgtest
""",
"pkgtest-clash": """[package]
id=pkgtest-clash
title=Package test: a clashing system
description=A throwaway recipe whose system is named like NES's
category=system
license=GPL-2.0
version=1

[debian]
apt=libretro-nestopia pspexp-retro
core=nestopia_libretro.so

[system]
id=nes
name=Clash
short=CLASH
ext=clash
roms=~/roms/clash
""",
}


def catalogue(out_dir, serial, tamper=False, v2=False):
    os.makedirs(out_dir)
    cat = os.path.join(out_dir, "catalogue.tar.gz")
    with tempfile.TemporaryDirectory() as w:
        shutil.copytree(os.path.join(ROOT, "recipes"), os.path.join(w, "recipes"))
        for rid, text in TEST_RECIPES.items():
            if v2 and rid == "pkgtest-hello":   # the recipe changes: version 2, figlet instead of hello (§5.6)
                text = text.replace("version=1", "version=2").replace("apt=hello", "apt=figlet").replace("exec=/usr/bin/hello", "exec=/usr/bin/figlet")
            os.makedirs(os.path.join(w, "recipes", rid))
            open(os.path.join(w, "recipes", rid, "recipe"), "w").write(text)
        open(os.path.join(w, "serial"), "w").write(serial + "\n")
        with tarfile.open(cat, "w:gz") as t:
            t.add(os.path.join(w, "recipes"), arcname="recipes")
            t.add(os.path.join(w, "serial"), arcname="serial")
    subprocess.run(["gpg", "--homedir", GNUPG, "--batch", "--yes", "--local-user", KEY, "--detach-sign", "-o", cat + ".sig", cat], check=True, capture_output=True)
    if tamper:   # one byte changed after signing
        b = bytearray(open(cat, "rb").read()); b[len(b) // 2] ^= 0xff; open(cat, "wb").write(bytes(b))


def main():
    host = sys.argv[sys.argv.index("--host") + 1] if "--host" in sys.argv else sys.exit(__doc__)
    serial = time.strftime("%Y%m%d%H%M%S", time.gmtime())
    with tempfile.TemporaryDirectory() as w:
        repo = os.path.join(w, "pkgtest-repo")
        catalogue(os.path.join(repo, "good"), serial)
        catalogue(os.path.join(repo, "tampered"), serial, tamper=True)
        catalogue(os.path.join(repo, "older"), "20000101000000")
        catalogue(os.path.join(repo, "good2"), str(int(serial) + 1), v2=True)
        subprocess.run(["ssh", host, "rm -rf /tmp/pkgtest-repo"], check=True)
        subprocess.run(["scp", "-rq", repo, os.path.join(ROOT, "tests/pkg/failures.sh"), "%s:/tmp/" % host], check=True)
    r = subprocess.run(["ssh", host, "sh /tmp/failures.sh; r=$?; rm -rf /tmp/pkgtest-repo /tmp/failures.sh; exit $r"])
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
