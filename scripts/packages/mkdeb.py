#!/usr/bin/env python3
"""
A .deb from a folder, without dpkg (the Mac has none): the ar archive dpkg
reads, holding debian-binary, control.tar.gz and data.tar.gz.

  mkdeb.py ROOT CONTROL OUT.deb
ROOT is the file tree as installed (ROOT/usr/...); CONTROL is the control
file (Package, Version, Architecture, Maintainer, Description, ...);
Installed-Size is added. Files are owned by root; modes kept.
"""
import gzip, hashlib, io, os, sys, tarfile, time


def tar_gz(members):
    """members: [(name, TarInfo, fileobj or None)] -> gzip'd tar bytes (deterministic)"""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.GNU_FORMAT) as t:
        for name, info, f in members:
            t.addfile(info, f)
    return gzip.compress(raw.getvalue(), mtime=0)


def info(name, size=0, mode=0o644, kind=tarfile.REGTYPE, link=""):
    ti = tarfile.TarInfo(name)
    ti.size, ti.mode, ti.type, ti.linkname = size, mode, kind, link
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = "root"
    ti.mtime = int(os.environ.get("SOURCE_DATE_EPOCH", time.time()))
    return ti


def data_members(root):
    out = [("./", info("./", mode=0o755, kind=tarfile.DIRTYPE), None)]
    total = 0
    for d, dirs, files in os.walk(root):
        dirs.sort()
        rel = os.path.relpath(d, root)
        if rel != ".":
            out.append((rel, info("./" + rel + "/", mode=0o755, kind=tarfile.DIRTYPE), None))
        for fn in sorted(files):
            p = os.path.join(d, fn)
            r = "./" + os.path.relpath(p, root)
            if os.path.islink(p):
                out.append((r, info(r, kind=tarfile.SYMTYPE, link=os.readlink(p)), None))
                continue
            data = open(p, "rb").read()
            total += len(data)
            mode = 0o755 if os.access(p, os.X_OK) else 0o644
            out.append((r, info(r, len(data), mode), io.BytesIO(data)))
    return out, total


def ar(entries):
    """entries: [(name, bytes)] -> the classic ar archive (names up to 16 bytes)"""
    out = io.BytesIO()
    out.write(b"!<arch>\n")
    for name, data in entries:
        hdr = "%-16s%-12d%-6d%-6d%-8s%-10d`\n" % (name, int(time.time()), 0, 0, "100644", len(data))
        out.write(hdr.encode())
        out.write(data)
        if len(data) % 2:
            out.write(b"\n")
    return out.getvalue()


def main():
    root, control, outp = sys.argv[1:4]
    members, total = data_members(root)
    ctl = open(control).read().rstrip("\n") + "\nInstalled-Size: %d\n" % ((total + 1023) // 1024)
    md5 = "".join("%s  %s\n" % (hashlib.md5(f.getvalue()).hexdigest(), n[2:]) for n, i, f in members if f is not None)
    for f in (m[2] for m in members if m[2] is not None):
        f.seek(0)
    cmembers = [("./", info("./", mode=0o755, kind=tarfile.DIRTYPE), None),
                ("./control", info("./control", len(ctl.encode())), io.BytesIO(ctl.encode())),
                ("./md5sums", info("./md5sums", len(md5.encode())), io.BytesIO(md5.encode()))]
    # maintainer scripts: preinst / postinst / prerm / postrm beside the control file, when there
    for name in ("preinst", "postinst", "prerm", "postrm"):
        sp = os.path.join(os.path.dirname(control), name)
        if os.path.isfile(sp):
            sd = open(sp, "rb").read()
            cmembers.append(("./" + name, info("./" + name, len(sd), 0o755), io.BytesIO(sd)))
    cdata = tar_gz(cmembers)
    ddata = tar_gz(members)
    with open(outp, "wb") as f:
        f.write(ar([("debian-binary", b"2.0\n"), ("control.tar.gz", cdata), ("data.tar.gz", ddata)]))
    print("%s: %d KB" % (outp, os.path.getsize(outp) // 1024))


if __name__ == "__main__":
    main()
