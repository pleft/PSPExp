#!/usr/bin/env python3
"""
PSPExp integration tests on a real PSP (or the stand-in), through the
console's test-control port (tools/pspexp-term/control.c).

    tests/psp/run.py --host user@board            all suites, on the board's PSP
    tests/psp/run.py --host user@board stick files only these suites
    tests/psp/run.py --local                      a console on this machine
                                                  (PSPEXP_TERM_CONTROL for its socket)

What it needs: the board's console running (pspexp-term --launcher) with the
PSP connected and a runtime that shares its memory stick. Nothing is pressed
by hand. The console refuses Restart, Power off and the Wi-Fi switch while a
run is on ("end" at the finish lifts that).

Output: build/psp-tests/<time>/report.md with each check, the transfer
speeds, and the PSP's own screenshots (PNG).
"""
import argparse, datetime, os, shlex, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STICK_DIR = "ms0:/PSPEXPIT"          # the tests' folder on the memory stick
BOARD_DIR = "/tmp/pspexp-it"          # and on the board


class Fail(Exception):
    pass


class Target:
    """The board (over ssh, one shared connection) or this machine."""

    def __init__(self, host, out):
        self.host, self.out = host, out
        self.ssh = []
        if host:
            sock = "/tmp/pspexp-it-ssh-%d" % os.getpid()
            self.ssh = ["ssh", "-o", "BatchMode=yes", "-o", "ControlMaster=auto", "-o", "ControlPath=" + sock,
                        "-o", "ControlPersist=120", host]
        self.term = "pspexp-term" if host else os.path.join(ROOT, "build/host/pspexp-term")

    def sh(self, cmd, check=True, timeout=120):
        argv = self.ssh + [cmd] if self.host else ["sh", "-c", cmd]
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        if check and r.returncode != 0:
            raise Fail("board command failed: %s\n%s" % (cmd, (r.stderr or r.stdout).strip()))
        return r.stdout

    def ctl(self, cmd, timeout=90):
        """(ok, lines) of one control command."""
        line = "%s --ctl %s --ctl-timeout %d" % (self.term, shlex.quote(cmd), timeout)
        argv = self.ssh + [line] if self.host else ["sh", "-c", line]
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout + 15)
        if r.returncode == 2:
            raise Fail("no console answers: %s" % (r.stderr.strip() or r.stdout.strip()))
        lines = r.stdout.rstrip("\n").split("\n")
        return r.returncode == 0, lines

    def fetch(self, remote, local):
        if self.host:
            subprocess.run(["scp", "-q", "-o", "ControlPath=/tmp/pspexp-it-ssh-%d" % os.getpid(), "%s:%s" % (self.host, remote), local], check=True)
        else:
            shutil.copy(remote, local)

    def put(self, local, remote):
        if self.host:
            subprocess.run(["scp", "-q", "-o", "ControlPath=/tmp/pspexp-it-ssh-%d" % os.getpid(), local, "%s:%s" % (self.host, remote)], check=True)
        else:
            shutil.copy(local, remote)


class Run:
    def __init__(self, target, out):
        self.t, self.out = target, out
        self.results, self.notes, self.shots = [], [], []

    # ---- steps ------------------------------------------------------------------
    def ctl(self, cmd, timeout=90):
        ok, lines = self.t.ctl(cmd, timeout)
        if not ok:
            raise Fail("%s -> %s" % (cmd, " / ".join(lines)))
        return lines

    def press(self, buttons, ms=None, idle=True):
        self.ctl("press %s%s" % (buttons, " %d" % ms if ms else ""))
        if idle:
            self.ctl("idle 2500")

    def state(self):
        ok, lines = self.t.ctl("state")
        if not ok:
            raise Fail("state -> %s" % " / ".join(lines))
        s = {}
        for l in lines[1:]:
            if "=" in l:
                k, v = l.split("=", 1)
                s[k] = v
        return s

    def expect(self, cond, what):
        if not cond:
            raise Fail(what)

    def shot(self, name, caption):
        lines = self.ctl("shot " + name, timeout=30)
        remote = lines[0].split()[1]
        bmp = os.path.join(self.out, name + ".bmp")
        self.t.fetch(remote, bmp)
        png = os.path.join(self.out, name + ".png")
        if shutil.which("sips"):
            subprocess.run(["sips", "-s", "format", "png", bmp, "--out", png], capture_output=True)
            os.remove(bmp)
        else:
            png = bmp
        self.shots.append((os.path.basename(png), caption))

    def note(self, s):
        self.notes.append(s)
        print("    " + s)

    def check(self, name, fn):
        t0 = time.time()
        try:
            fn()
            self.results.append((name, True, "", time.time() - t0))
            print("  PASS %s (%.1f s)" % (name, time.time() - t0))
        except (Fail, AssertionError, subprocess.SubprocessError, OSError, IndexError, KeyError, ValueError) as e:
            self.results.append((name, False, str(e), time.time() - t0))
            print("  FAIL %s: %s" % (name, e))

    # ---- helpers for the views ----------------------------------------------------------
    def to_category(self, name):
        s = self.state()
        cats = s["launcher.categories"].split("|")
        self.expect(name in cats, "no %s section (sections: %s)" % (name, ", ".join(cats)))
        for _ in range(len(cats) + 2):
            s = self.state()
            cur = s["launcher.category"]
            if cur == name:
                return s
            self.press("right" if cats.index(name) > cats.index(cur) else "left", idle=False)
        self.ctl("idle 2500")
        raise Fail("could not reach %s" % name)

    def select(self, side, name, limit=80):
        """highlight name in a file-manager panel"""
        key = "files.%s.selected" % side
        names = [n.rstrip("/") for n in self.state()["files.%s.names" % side].split("|") if n]
        self.expect(name in names, "%s not in the %s panel (%s)" % (name, side, ", ".join(names)))
        for _ in range(limit):
            s = self.state()
            if s[key] == name:
                return
            cur = s[key]
            here = names.index(cur) if cur in names else -1
            self.press("down" if names.index(name) > here else "up", idle=False)
        raise Fail("could not highlight %s" % name)

    def panel_drive(self, side, drive):
        """the panel onto drive (its drop-down)"""
        self.press("l" if side == "left" else "r")
        for _ in range(100):   # up to the drive button
            if self.state()["files.%s.on_drives" % side] == "1":
                break
            self.press("up", idle=False)
        self.press("cross")
        s = self.state()
        self.expect(s["files.overlay"] == "drives", "the drive list did not open (overlay %s)" % s["files.overlay"])
        drives = s["files.drives"].split("|")
        self.expect(drive in drives, "no %s in the drive list (%s)" % (drive, ", ".join(drives)))
        for _ in range(len(drives) + 1):
            if int(self.state()["files.menu_sel"]) == drives.index(drive):
                break
            self.press("down", idle=False)
        self.press("cross")
        s = self.state()
        self.expect(s["files.%s.drive" % side] == drive, "the %s panel is on %s, not %s" % (side, s["files.%s.drive" % side], drive))
        return s

    def wait_job(self, limit=120):
        end = time.time() + limit
        while time.time() < end:
            s = self.state()
            if s["files.overlay"] != "progress":
                return s
            time.sleep(0.5)
        raise Fail("the job did not finish in %d s" % limit)


# ---- suites -----------------------------------------------------------------------------

def suite_link(r):
    def connected():
        s = r.state()
        r.expect(s["connected"] == "1", "the PSP is not connected")
        r.expect(s["psp_files"] == "1", "the PSP does not share its memory stick (older runtime?)")
    r.check("the PSP is connected and shares its memory stick", connected)

    def errors():
        st = dict(l.split("=", 1) for l in r.ctl("stats")[1:] if "=" in l)
        r.expect(int(st["protocol_errors"]) == 0, "protocol errors: %s" % st["protocol_errors"])
        r.expect(int(st["resource_errors"]) == 0, "resource errors: %s" % st["resource_errors"])
        r.expect(int(st["frames_presented"]) > 0, "no frame presented yet")
        r.note("frames presented %s, last present latency %s us, GE %s us" % (st["frames_presented"], st["last_present_latency_us"], st["last_ge_us"]))
    r.check("no link errors, frames reach the screen", errors)

    def screenshot():
        t0 = time.time()
        r.shot("link-screen", "The PSP's screen when the run began")
        r.note("a screenshot round trip: %.1f s" % (time.time() - t0))
    r.check("a screenshot from the PSP comes back", screenshot)


def suite_stick(r):
    t = r.t
    t.sh("rm -rf %s && mkdir -p %s" % (BOARD_DIR, BOARD_DIR))
    r.t.ctl("fs rmtree " + STICK_DIR)   # a previous run's, if any

    def space():
        line = r.ctl("fs df")[0]
        kv = dict(p.split("=") for p in line.split()[1:])
        fr, tot = int(kv["free"]), int(kv["total"])
        r.expect(0 < tot and fr <= tot, "free %d of %d" % (fr, tot))
        r.note("memory stick: %.0f MB free of %.0f MB" % (fr / 1048576, tot / 1048576))
    r.check("free space is sensible", space)

    def roundtrip():
        size = 4 * 1024 * 1024
        t.sh("head -c %d /dev/urandom > %s/r.bin" % (size, BOARD_DIR))
        r.ctl("fs mkdir " + STICK_DIR)
        up = r.ctl("fs put %s/r.bin %s/r.bin" % (BOARD_DIR, STICK_DIR), timeout=120)[0]
        down = r.ctl("fs get %s/r.bin %s/back.bin" % (STICK_DIR, BOARD_DIR), timeout=120)[0]
        t.sh("cmp %s/r.bin %s/back.bin" % (BOARD_DIR, BOARD_DIR))
        r.note("4 MB to the stick: %s; back: %s" % (up.split(" ", 1)[1], down.split(" ", 1)[1]))
    r.check("4 MB to the stick and back, identical", roundtrip)

    def listing():
        rows = r.ctl("fs ls " + STICK_DIR)[1:]
        entry = [x for x in rows if x.startswith("r.bin ")]
        r.expect(entry, "r.bin not listed: %s" % rows)
        name, size, mtime = entry[0].split()
        r.expect(int(size) == 4 * 1024 * 1024, "size %s" % size)
        board_now = int(t.sh("date +%s").strip())
        # the date is the PSP's own clock (local time, read as UTC): a real date; a
        # clock far from the board's is the PSP's setting, reported, not a failure
        r.expect(int(mtime) > 946684800, "no date (%s)" % mtime)
        off = (int(mtime) - board_now) / 3600.0
        if abs(off) > 26:
            r.note("the PSP's clock is %.1f days %s the board's: set it in the PSP's Settings > Date and Time" % (abs(off) / 24, "behind" if off < 0 else "ahead of"))
        else:
            r.note("the PSP's clock is within a day of the board's (%+.1f h, time zones included)" % off)
    r.check("the listing shows the file, its size and a sensible date", listing)

    def rename():
        r.ctl("fs mv %s/r.bin %s/s.bin" % (STICK_DIR, STICK_DIR))
        rows = r.ctl("fs ls " + STICK_DIR)[1:]
        r.expect(any(x.startswith("s.bin ") for x in rows) and not any(x.startswith("r.bin ") for x in rows), "after mv: %s" % rows)
    r.check("rename on the stick", rename)

    def errors():
        ok, lines = t.ctl("fs rm %s/nothing-here" % STICK_DIR)
        r.expect(not ok, "removing a missing file succeeded")
        ok, lines = t.ctl("fs mkdir " + STICK_DIR)
        r.expect(not ok, "mkdir of an existing folder succeeded")
        ok, lines = t.ctl("fs ls ms0:/PSP/../../")
        r.expect(not ok, "a path with .. was accepted")
        ok, lines = t.ctl("fs rm ms0:/PSP")
        r.expect(not ok, "a folder with files in it was removed")
    r.check("errors: missing file, existing folder, .. paths, non-empty folder", errors)

    def cleanup():
        r.ctl("fs rmtree " + STICK_DIR)
        ok, _ = t.ctl("fs ls " + STICK_DIR)
        r.expect(not ok, "the test folder is still there")
    r.check("clean-up on the stick", cleanup)


def suite_launcher(r):
    def sections():
        r.ctl("open launcher")
        r.ctl("idle 3000")
        s = r.state()
        cats = s["launcher.categories"].split("|")
        r.expect("Apps" in cats and "System" in cats, "sections: %s" % cats)
        r.note("sections: " + ", ".join(cats))
    r.check("the launcher's sections", sections)

    def apps():
        s = r.to_category("Apps")
        items = s["launcher.items"].split("|")
        r.expect(items[:2] == ["Terminal", "Files"], "Apps starts with %s" % items[:2])
        r.shot("launcher-apps", "Apps: Terminal and Files, built in")
    r.check("Apps starts with Terminal and Files", apps)

    def walk():
        cats = r.state()["launcher.categories"].split("|")
        for c in cats:
            s = r.to_category(c)
            r.ctl("idle 2500")
            r.shot("launcher-%s" % c.lower().replace(" ", "-"), "Launcher: %s (%s)" % (c, s["launcher.selected"] or "empty"))
    r.check("every section draws (screenshots)", walk)


def suite_files(r):
    t = r.t
    t.sh("rm -rf %s/board && mkdir -p %s/board" % (BOARD_DIR, BOARD_DIR))
    r.t.put(os.path.join(ROOT, "scripts/packages/art/cavestory.png"), "%s/board/pic.png" % BOARD_DIR)
    t.sh("printf 'PSPExp integration test\\nline two\\n' > %s/board/note.txt" % BOARD_DIR)
    r.t.ctl("fs rmtree " + STICK_DIR)
    r.ctl("fs mkdir " + STICK_DIR)

    def open_board():
        r.ctl("open files %s/board" % BOARD_DIR)
        r.ctl("idle 3000")
        s = r.state()
        r.expect(s["view"] == "files", "view %s" % s["view"])
        r.expect(s["files.left.path"] == BOARD_DIR + "/board", "left panel at %s" % s["files.left.path"])
    r.check("the file manager opens at a board folder", open_board)

    def to_psp():
        r.panel_drive("right", "PSP")
        r.select("right", "PSPEXPIT")
        r.press("cross")
        s = r.state()
        r.expect(s["files.right.path"] == STICK_DIR, "right panel at %s" % s["files.right.path"])
        r.shot("files-board-and-psp", "The file manager: the board's folder and the PSP's memory stick")
    r.check("the right panel onto the PSP's memory stick (drive list)", to_psp)

    def copy_to_psp():
        for name in ("pic.png", "note.txt"):
            r.press("l")
            r.select("left", name)
            r.press("triangle")
            s = r.state()
            r.expect(s["files.overlay"] == "actions", "actions menu did not open (%s)" % s["files.overlay"])
            r.press("cross")   # Copy to the other panel
            s = r.wait_job()
            r.expect(s["files.overlay"] == "none", "after the copy: %s %s" % (s.get("files.message.title", ""), s.get("files.message.text", "")))
        rows = r.ctl("fs ls " + STICK_DIR)[1:]
        size = int(t.sh("wc -c < %s/board/pic.png" % BOARD_DIR).strip())
        r.expect(any(x.split()[0] == "pic.png" and int(x.split()[1]) == size for x in rows), "pic.png on the stick: %s" % rows)
        r.ctl("fs get %s/pic.png %s/pic-back.png" % (STICK_DIR, BOARD_DIR))
        t.sh("cmp %s/board/pic.png %s/pic-back.png" % (BOARD_DIR, BOARD_DIR))
    r.check("copy two files from the board to the PSP (triangle, Copy)", copy_to_psp)

    def picture():
        r.press("r")
        r.select("right", "pic.png")
        r.press("cross")
        s = r.state()
        r.expect(s["files.overlay"] == "image", "overlay %s %s" % (s["files.overlay"], s.get("files.message.text", "")))
        r.shot("files-picture-from-psp", "A picture opened straight from the PSP's memory stick")
        r.press("circle")
    r.check("a picture on the stick opens full screen", picture)

    def text():
        r.select("right", "note.txt")
        r.press("cross")
        s = r.state()
        r.expect(s["files.overlay"] == "view" and "note.txt" in s.get("files.viewer", ""), "overlay %s" % s["files.overlay"])
        r.shot("files-text-from-psp", "A text file read from the PSP's memory stick")
        r.press("circle")
    r.check("a text file on the stick opens in the viewer", text)

    def delete():
        r.select("right", "pic.png")
        r.press("triangle")
        r.press("down"); r.press("down")   # Delete
        r.press("cross")
        s = r.state()
        r.expect(s["files.overlay"] == "confirm", "no question before deleting (%s)" % s["files.overlay"])
        r.press("cross")
        r.wait_job()
        rows = r.ctl("fs ls " + STICK_DIR)[1:]
        r.expect(not any(x.startswith("pic.png ") for x in rows), "pic.png still on the stick")
    r.check("delete on the stick asks, then deletes", delete)

    def back():
        r.panel_drive("right", "Board")
        r.ctl("fs rmtree " + STICK_DIR)
        t.sh("rm -rf " + BOARD_DIR)
    r.check("back to the board, clean-up", back)


def suite_terminal(r):
    def echo():
        r.ctl("open terminal")
        r.ctl("idle 2500")
        r.ctl("type echo PSPEXP_$((6*7))\\n")
        time.sleep(0.8)
        s = r.state()
        text = [v for k, v in s.items() if k.startswith("term.") and k[5:].isdigit()]
        r.expect(any("PSPEXP_42" in l and "echo" not in l for l in text), "no output line PSPEXP_42 on screen")
        r.shot("terminal-echo", "The terminal: a command's output")
    r.check("a command in the terminal and its output", echo)


SUITES = [("link", suite_link), ("stick", suite_stick), ("launcher", suite_launcher),
          ("files", suite_files), ("terminal", suite_terminal)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", help="user@board whose console drives the PSP")
    ap.add_argument("--local", action="store_true", help="a console on this machine")
    ap.add_argument("suites", nargs="*", help="some of: " + " ".join(n for n, _ in SUITES))
    a = ap.parse_args()
    if not a.host and not a.local:
        ap.error("--host user@board or --local")
    want = a.suites or [n for n, _ in SUITES]
    out = os.path.join(ROOT, "build/psp-tests", datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out)
    r = Run(Target(a.host, out), out)
    ok, lines = r.t.ctl("ping")
    if not ok:
        sys.exit("the console does not answer: %s" % lines)
    t0 = time.time()
    try:
        for name, fn in SUITES:
            if name in want:
                print("%s:" % name)
                fn(r)
    finally:
        try:
            r.t.ctl("open launcher"); r.t.ctl("end")
        except Exception:
            pass
    passed = sum(1 for x in r.results if x[1])
    with open(os.path.join(out, "report.md"), "w") as f:
        f.write("# PSPExp integration tests\n\n%s, target %s, %d of %d passed, %.0f s\n\n" %
                (datetime.datetime.now().strftime("%Y-%m-%d %H:%M"), a.host or "this machine", passed, len(r.results), time.time() - t0))
        f.write("| | check | time | detail |\n|---|---|---|---|\n")
        for name, good, err, dt in r.results:
            f.write("| %s | %s | %.1f s | %s |\n" % ("PASS" if good else "**FAIL**", name, dt, err.replace("|", "/").replace("\n", " ")))
        if r.notes:
            f.write("\n## Measured\n\n" + "".join("- %s\n" % n for n in r.notes))
        if r.shots:
            f.write("\n## The PSP's screen\n\n" + "".join("![%s](%s)\n\n%s\n\n" % (c, p, c) for p, c in r.shots))
    print("\n%d of %d passed; report: %s" % (passed, len(r.results), os.path.join(out, "report.md")))
    sys.exit(0 if passed == len(r.results) else 1)


if __name__ == "__main__":
    main()
