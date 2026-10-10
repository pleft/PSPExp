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

    def __init__(self, host, out, known_hosts=None):
        self.host, self.out = host, out
        self.ssh = []
        if host:
            sock = "/tmp/pspexp-it-ssh-%d" % os.getpid()
            self.ssh = ["ssh", "-o", "BatchMode=yes", "-o", "ControlMaster=auto", "-o", "ControlPath=" + sock,
                        "-o", "ControlPersist=120"] + (["-o", "UserKnownHostsFile=" + known_hosts] if known_hosts else []) + [host]
        self.term = "pspexp-term" if host else os.path.join(ROOT, "build/host/pspexp-term")

    def sh(self, cmd, check=True, timeout=120):
        argv = self.ssh + [cmd] if self.host else ["sh", "-c", cmd]
        try:
            r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:   # a board out of reach (a weak Wi-Fi link): this check fails, the run goes on
            raise Fail("board command timed out after %d s: %s" % (timeout, cmd[:80]))
        if check and r.returncode != 0:
            raise Fail("board command failed: %s\n%s" % (cmd, (r.stderr or r.stdout).strip()))
        return r.stdout

    def ctl(self, cmd, timeout=90):
        """(ok, lines) of one control command."""
        line = "%s --ctl %s --ctl-timeout %d" % (self.term, shlex.quote(cmd), timeout)
        argv = self.ssh + [line] if self.host else ["sh", "-c", line]
        try:
            r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout + 15)
        except subprocess.TimeoutExpired:
            raise Fail("console did not answer within %d s: %s" % (timeout + 15, cmd[:60]))
        if r.returncode == 2:
            raise Fail("no console answers: %s" % (r.stderr.strip() or r.stdout.strip()))
        lines = r.stdout.rstrip("\n").split("\n")
        return r.returncode == 0, lines

    def fetch(self, remote, local):
        """a board's file here, compressed on the way (a BMP shrinks ninefold: it
        matters on a slow Wi-Fi like the GameShell's)"""
        if self.host:
            import gzip
            r = subprocess.run(self.ssh + ["gzip -c " + shlex.quote(remote)], capture_output=True, timeout=120)
            if r.returncode != 0:
                raise Fail("cannot fetch %s: %s" % (remote, r.stderr.decode(errors="replace").strip()))
            with open(local, "wb") as f:
                f.write(gzip.decompress(r.stdout))
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
        ok, _ = self.t.ctl("category " + name)   # straight there (older consoles: by the buttons below)
        if ok:
            # (the Omega can take longer than an idle to show it: its covers loading)
            for _ in range(6):
                self.ctl("idle 2500")
                s = self.state()
                if s["launcher.category"] == name:
                    return s
            raise Fail("asked for %s, still in %s" % (name, s["launcher.category"]))
        for _ in range(len(cats) + 2):
            s = self.state()
            cur = s["launcher.category"]
            if cur == name:
                return s
            self.press("down" if cats.index(name) > cats.index(cur) else "up", idle=False)   # the rail runs down the left
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

    def clock():
        line = r.ctl("clock")[0]
        kv = dict(p.split("=") for p in line.split()[1:])
        r.expect(int(kv["result"]) >= 0 and int(kv["zone_result"]) >= 0, "the PSP could not set its clock or zone: %s" % line)
        r.expect(abs(int(kv["off_s"])) <= 90 or kv["board_synced"] == "0", "the PSP's clock is %s s off" % kv["off_s"])
        r.expect(kv["psp_tz"] == kv["board_tz"] or kv["board_zone"] == "0", "time zone: PSP %s min, board %s min" % (kv["psp_tz"], kv["board_tz"]))
        r.note("clock: PSP %+d s from the board, zone UTC%+.1f on both" % (int(kv["off_s"]), int(kv["board_tz"]) / 60))
    r.check("the PSP's clock and time zone follow the board", clock)

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
        z = t.sh("date +%z").strip()   # the stick's dates are the PSP's local time: the board's offset, when their zones agree
        tz_s = (1 if z[0] == "+" else -1) * (int(z[1:3]) * 3600 + int(z[3:5]) * 60)
        off = int(mtime) - board_now - tz_s
        r.expect(abs(off) < 180, "a new file's date is %.1f h from the board's local time (the PSP's clock or zone is off)" % (off / 3600.0))
        r.note("a new file on the stick carries the board's local time (%+d s)" % off)
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
        r.expect("Apps" in cats and "Settings" in cats, "sections: %s" % cats)
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


def suite_relink(r):
    """the link dropped (as a cable pull) and back, three times"""
    def relink():
        times = []
        for i in range(3):
            t0 = time.time()
            r.ctl("relink", timeout=40)
            times.append(time.time() - t0)
            s = r.state()
            r.expect(s["connected"] == "1" and s["psp_files"] == "1", "after relink %d: not back fully" % (i + 1))
        r.note("the link back after a drop: %s s" % ", ".join("%.1f" % x for x in times))
    r.check("the link drops and comes back (3 times)", relink)

    def after():
        s = r.state()
        r.expect(s.get("psp_clock", "").startswith("result="), "no clock check after reconnecting")
        st = dict(l.split("=", 1) for l in r.ctl("stats")[1:] if "=" in l)
        r.expect(int(st["protocol_errors"]) == 0, "protocol errors: %s" % st["protocol_errors"])
        r.ctl("open launcher")
        r.ctl("idle 3000")
        r.shot("relink-launcher", "The launcher after three dropped links")
    r.check("after reconnecting: clock checked, no errors, the screen draws", after)


LOG = "/tmp/pspexp-launcher.log"   # where the console writes the programs it starts


def play(r, category, title, min_fps, name):
    """start title from the launcher, read its frame rates for a while, stop it, check the launcher is back"""
    t = r.t
    r.ctl("open launcher")
    r.to_category(category)
    r.ctl("select " + title)
    r.ctl("idle 2500")
    off = int(t.sh("wc -c < %s 2>/dev/null || echo 0" % LOG).strip() or 0)
    r.press("cross", idle=False)
    end = time.time() + 10
    while time.time() < end and r.state()["running"] != title:
        time.sleep(0.3)
    r.expect(r.state()["running"] == title, "%s did not start" % title)
    fps, errors, underruns, end = [], [], [], time.time() + 75
    while time.time() < end:   # the SDL layer's line every few seconds: a starting phase, then three to judge by
        time.sleep(2)
        lines = t.sh("tail -c +%d %s 2>/dev/null | grep -a 'fps  ' || true" % (off + 1, LOG)).splitlines()
        if len(lines) >= 4:
            break
    r.expect(lines, "%s wrote no frame rates in 75 s (log: %s)" % (title, t.sh("tail -c +%d %s | tail -3" % (off + 1, LOG)).strip()))
    for l in lines[1:] or lines:
        w = l.split()
        try:
            fps.append(float(w[w.index("fps") - 1]))
            errors.append(int(w[w.index("errors") + 1]))
            underruns.append(int(w[w.index("underruns") + 1]))
        except (ValueError, IndexError):
            pass
    r.expect(fps, "no frame rate in: %s" % lines[-1])
    fps.sort()
    med = fps[len(fps) // 2]
    r.note("%s: %.0f fps (%.0f-%.0f), link errors %d, audio underruns %d" % (title, med, fps[0], fps[-1], max(errors or [0]), max(underruns or [0])))
    r.ctl("stop", timeout=40)
    s = r.state()
    r.expect(s["connected"] == "1" and s["running"] == "" and s["view"] == "launcher", "after quitting: view %s, connected %s" % (s["view"], s["connected"]))
    r.ctl("idle 3000")
    r.shot("after-" + name, "The launcher back after %s" % title)
    # the best rate seen: an intro that waits on a logo draws few frames on purpose (OpenTyrian's), the speed shows when it moves
    r.expect(fps[-1] >= DRAWS_FPS, "%s draws no frames" % title)
    if min_fps:
        r.expect(fps[-1] >= min_fps, "%s reaches only %.0f fps (--min-fps %d)" % (title, fps[-1], min_fps))
    r.expect(max(errors or [0]) == 0, "%s: link errors" % title)


# Frame rates differ a lot between boards (the Omega's MIPS vs the Pi 400):
# they are recorded, not judged. A game passes when it starts, draws, has no
# link errors and quits back. --min-fps N adds a floor, for one board at a time.
MIN_FPS = 0
DRAWS_FPS = 1.0   # "it draws": at least this, at its best moment


def slug(s):
    return "".join(c if c.isalnum() else "-" for c in s.lower()).strip("-")


def suite_games(r):
    """whatever this board has: every installed game, and the first ROM of
    every emulated system that has ROMs (a system without any is not listed)"""
    r.ctl("open launcher")
    r.ctl("idle 3000")
    s = r.state()
    cats = s["launcher.categories"].split("|")
    systems = [x for x in s.get("launcher.systems", "").split("|") if x]
    games = [x for x in r.to_category("Games")["launcher.items"].split("|") if x] if "Games" in cats else []
    r.note("games installed: %s" % (", ".join(games) or "none"))
    r.note("systems with ROMs: %s" % (", ".join(systems) or "none"))
    for title in games:
        r.check("%s starts, draws, quits back to the launcher" % title,
                lambda title=title: play(r, "Games", title, MIN_FPS, slug(title)))
    for cat in systems:
        roms = [x for x in r.to_category(cat)["launcher.items"].split("|") if x]
        if not roms:
            continue
        r.check("%s: its first ROM (%s) starts, draws, quits back" % (cat, roms[0]),
                lambda cat=cat, first=roms[0]: play(r, cat, first, MIN_FPS, "emu-" + slug(cat)))


STORE_WRITES = False   # --store-writes: the Packages app also removes and reinstalls a package


def suite_apps(r):
    t = r.t

    def listed():
        r.ctl("open launcher")
        s = r.to_category("Apps")
        items = s["launcher.items"].split("|")
        r.expect(items[:4] == ["Terminal", "Files", "System Monitor", "Packages"], "Apps starts with %s" % items[:4])
    r.check("Apps lists Terminal, Files, System Monitor and Packages", listed)

    def monitor():
        r.ctl("open monitor")
        r.ctl("wait 2500")   # rates and CPU shares need two readings
        s = r.state()
        r.expect(s["view"] == "monitor", "view %s" % s["view"])
        r.expect(int(s["monitor.cores"]) >= 1 and int(s["monitor.mem_total"]) > 0 and int(s["monitor.programs"]) > 0,
                 "no figures: cores %s, memory %s, programs %s" % (s["monitor.cores"], s["monitor.mem_total"], s["monitor.programs"]))
        r.note("monitor: %s cores, %.0f MB, %s programs, CPU %s%%%s" % (s["monitor.cores"], int(s["monitor.mem_total"]) / 1048576, s["monitor.programs"], s["monitor.cpu"],
               "" if s["monitor.temp_mc"] == "-1" else ", %.0f C" % (int(s["monitor.temp_mc"]) / 1000)))
        r.shot("monitor-overview", "System Monitor: the overview")
        r.press("r", idle=False); r.ctl("wait 1200")
        r.expect(r.state()["monitor.page"] == "programs", "the Programs page did not open")
        r.shot("monitor-programs", "System Monitor: the programs")
        r.press("circle", idle=False); r.ctl("wait 800")
        r.expect(r.state()["view"] == "launcher", "circle did not close it")
    r.check("System Monitor shows the board's figures, both pages, closes", monitor)

    def store():
        r.ctl("open store")
        r.ctl("wait 800")
        s = r.state()
        r.expect(s["view"] == "store", "view %s" % s["view"])
        end = time.time() + 30   # the list is read in the background, a "Reading" box meanwhile
        while time.time() < end and s.get("store.mode") == "loading": time.sleep(0.5); s = r.state()
        r.expect(s.get("store.mode") != "loading", "the list still loading after 30 s")
        pk = dict(x.split(":") for x in s["store.packages"].split("|") if ":" in x)
        installed = set(l.split()[0] for l in t.sh("pspexp-pkg list 2>/dev/null || true").splitlines() if l.strip())
        shown = set(k for k, v in pk.items() if v in ("installed", "update", "local"))
        r.expect(shown == installed, "installed per the app %s, per pspexp-pkg %s" % (sorted(shown), sorted(installed)))
        r.note("packages: %d offered or installed, %d installed, catalogue %s" % (len(pk), len(installed), "published with the builds" if s.get("store.local") == "1" else "apt's"))
        r.shot("store", "Packages")
    r.check("Packages lists what the repositories offer and what is installed", store)

    if STORE_WRITES:
        def cycle():
            s = r.state()
            pk = [x.split(":") for x in s["store.packages"].split("|") if ":" in x]
            cand = [k for k, v in pk if v == "installed"]
            r.expect(cand, "nothing installed that a repository also offers")
            sizes = {}
            for k in cand:   # the smallest: the quickest to put back
                sizes[k] = int(t.sh("du -sk \"${PSPEXP_ROOT:-/opt/pspexp}/apps/%s\" 2>/dev/null | cut -f1 || echo 0" % k).strip() or 0)
            pid = min(cand, key=lambda k: sizes[k] or 1 << 30)
            for _ in range(len(pk) + 1):
                if r.state()["store.selected"] == pid: break
                r.press("down", idle=False)
            r.press("square", idle=False); r.ctl("wait 500")
            r.expect(r.state()["store.mode"] == "confirm", "no question before removing")
            r.press("cross", idle=False)
            end = time.time() + 60
            while time.time() < end and r.state()["store.mode"] == "working": time.sleep(0.5)
            r.expect("Removed" in r.state()["store.result"], "remove: %s" % r.state()["store.result"])
            r.press("cross", idle=False)
            r.press("cross", idle=False)   # install it again
            end = time.time() + 120
            while time.time() < end and r.state()["store.mode"] == "working": time.sleep(0.5)
            r.expect("Installed" in r.state()["store.result"], "install: %s" % r.state()["store.result"])
            r.press("cross", idle=False)
            r.expect(pid in t.sh("pspexp-pkg list"), "%s is not installed again" % pid)
            r.note("packages: removed and reinstalled %s" % pid)
        r.check("Packages removes a package (asks) and installs it again", cycle)
    r.ctl("open launcher")


SUITES = [("link", suite_link), ("stick", suite_stick), ("launcher", suite_launcher),
          ("files", suite_files), ("terminal", suite_terminal), ("apps", suite_apps), ("relink", suite_relink), ("games", suite_games)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", help="user@board whose console drives the PSP")
    ap.add_argument("--local", action="store_true", help="a console on this machine")
    ap.add_argument("--known-hosts", help="an SSH known-hosts file of its own (a freshly flashed card: new host keys)")
    ap.add_argument("--term", help="the board's pspexp-term when it isn't on the PATH (the Omega: /mnt/mmcblk0p1/pspexp/pspexp-term)")
    ap.add_argument("--min-fps", type=int, default=0, help="games: also require this frame rate (off: rates are only recorded)")
    ap.add_argument("--store-writes", action="store_true", help="apps: the Packages app also removes and reinstalls the smallest package")
    ap.add_argument("suites", nargs="*", help="some of: " + " ".join(n for n, _ in SUITES))
    a = ap.parse_args()
    global MIN_FPS, STORE_WRITES
    MIN_FPS = a.min_fps
    STORE_WRITES = a.store_writes
    if not a.host and not a.local:
        ap.error("--host user@board or --local")
    want = a.suites or [n for n, _ in SUITES]
    out = os.path.join(ROOT, "build/psp-tests", datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out)
    tg = Target(a.host, out, a.known_hosts)
    if a.term: tg.term = a.term
    r = Run(tg, out)
    ok, lines = r.t.ctl("ping")
    if not ok:
        sys.exit("the console does not answer: %s" % lines)
    t0 = time.time()
    try:
        for name, fn in SUITES:
            if name in want:
                print("%s:" % name)
                try:
                    fn(r)
                except Fail as e:   # its setup failed (the board out of reach): the next suite still runs
                    r.results.append(("%s: setting up" % name, False, str(e), 0.0))
                    print("  FAIL %s: setting up: %s" % (name, e))
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
