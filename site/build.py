#!/usr/bin/env python3
"""
The web page at https://pspexp.dev (GOING_PUBLIC: required at launch), built into site/out/:
  python3 site/build.py
- index.html: the front page, from site/index.html (its {{...}} filled in here);
- download.html: the three downloads of the latest release, from site/download.html, with the
  numbers GitHub gives at build time (its own script then asks GitHub for live ones);
- manual/*.html: the user manual, from docs/manual/*.md, so it is written once (README.md becomes
  manual/index.html; links between its pages follow, links out of it go to GitHub);
- style.css, the screenshots (docs/images) and the launcher's font (Inter, OFL, beside its licence).
Python's standard library only: Cloudflare Pages runs this as the build command (output: site/out).
The Markdown converter knows what the manual uses: headings, paragraphs, lists (nested), tables,
fenced code, block quotes, rules, and inline code, bold, italics, links and images.
"""
import datetime, hashlib, html, json, os, re, shutil, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SITE = os.path.join(ROOT, "site")
OUT = os.path.join(SITE, "out")
REPO = "https://github.com/pleft/PSPExp"
RELEASES = REPO + "/releases/latest"
VIDEOS = [("https://www.youtube-nocookie.com/embed/CQm_w-4Oqfw", "Part I: the tour"),   # youtu.be/CQm_w-4Oqfw
          ("https://www.youtube-nocookie.com/embed/_rBYBBLUeNc", "Part II: the games"),   # youtu.be/_rBYBBLUeNc
          ("https://www.youtube-nocookie.com/embed/ex6nnDBRzHU", "Dreamcast on the PSP")]   # youtu.be/ex6nnDBRzHU
LAUNCHED = True    # until launch day: every page says "do not index" (Cloudflare Pages' _headers);
                   # then True, with pspexp.dev attached: pspexp.dev is indexed, pages.dev never is
HOSTS = ["pspexp.pages.dev", "www.pspexp.dev"]   # always "do not index": one address in search results
SOON = ["pspexp.dev"]                            # "do not index" until launch day

MANUAL = [("README.md", "index.html", "The manual"), ("psp.md", "psp.html", "The PSP"),
          ("raspberry-pi.md", "raspberry-pi.html", "Raspberry Pi"), ("gameshell.md", "gameshell.html", "GameShell"),
          ("omega2.md", "omega2.html", "Omega2+"), ("using-pspexp.md", "using-pspexp.html", "Using PSPExp"),
          ("recording.md", "recording.html", "Recording PSP games")]


# ---- Markdown, as the manual writes it --------------------------------------------------------

def slug(text):
    t = re.sub(r"<[^>]+>", "", text).strip().lower()
    t = re.sub(r"[^\w\- ]", "", t)
    return re.sub(r" ", "-", t)


def link_target(u):
    """a manual page links its neighbours (.md -> .html); anything else of the repository: GitHub"""
    if re.match(r"^[a-z]+:", u) or u.startswith("#"):
        return u
    path, _, frag = u.partition("#")
    frag = "#" + frag if frag else ""
    for md, page, _ in MANUAL:
        if path == md:
            return page + frag
    if path.startswith("../images/"):
        return "../images/" + os.path.basename(path)
    target = os.path.normpath(os.path.join("docs/manual", path))
    kind = "tree" if not os.path.splitext(target)[1] else "blob"
    return "%s/%s/main/%s%s" % (REPO, kind, target, frag)


def inline(t):
    codes = []
    def keep(m):
        codes.append("<code>%s</code>" % html.escape(m.group(1)))
        return "\x00%d\x00" % (len(codes) - 1)
    t = re.sub(r"`([^`]+)`", keep, t)
    t = html.escape(t, quote=False)
    t = re.sub(r"!\[([^\]]*)\]\(([^)\s]+)\)", lambda m: '<img src="%s" alt="%s" loading="lazy">' % (link_target(m.group(2)), m.group(1)), t)
    t = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", lambda m: '<a href="%s">%s</a>' % (link_target(m.group(2)), m.group(1)), t)
    t = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", t)
    t = re.sub(r"(?<![\w*])\*([^*\n]+)\*(?![\w*])", r"<em>\1</em>", t)
    t = re.sub(r"(?<![\w_])_([^_\n]+)_(?![\w_])", r"<em>\1</em>", t)
    return re.sub(r"\x00(\d+)\x00", lambda m: codes[int(m.group(1))], t)


LIST_ITEM = re.compile(r"^( *)([-*+]|\d+\.) +(.*)$")


def blocks(lines):
    """Markdown lines (their common indentation removed) -> HTML"""
    out, i, n = [], 0, len(lines)
    def para(buf):
        if buf:
            out.append("<p>%s</p>" % inline(" ".join(x.strip() for x in buf)))
    buf = []
    while i < n:
        line = lines[i]
        s = line.strip()
        if not s:
            para(buf); buf = []; i += 1; continue
        if s.startswith("```"):
            para(buf); buf = []
            lang = s[3:].strip()
            ind = len(line) - len(line.lstrip())
            code, i = [], i + 1
            while i < n and not lines[i].strip().startswith("```"):
                code.append(lines[i][ind:] if lines[i][:ind].strip() == "" else lines[i].lstrip())
                i += 1
            i += 1
            out.append('<pre><code%s>%s</code></pre>' % (' class="lang-%s"' % lang if lang else "", html.escape("\n".join(code))))
            continue
        m = re.match(r"^(#{1,4}) +(.*)$", s)
        if m:
            para(buf); buf = []
            lvl, text = len(m.group(1)), m.group(2)
            out.append('<h%d id="%s">%s</h%d>' % (lvl, slug(text), inline(text), lvl))
            i += 1; continue
        if re.match(r"^(-{3,}|\*{3,})$", s):
            para(buf); buf = []; out.append("<hr>"); i += 1; continue
        if s.startswith(">"):
            para(buf); buf = []
            q = []
            while i < n and lines[i].strip().startswith(">"):
                q.append(re.sub(r"^\s*> ?", "", lines[i])); i += 1
            out.append("<blockquote>%s</blockquote>" % blocks(q))
            continue
        if s.startswith("|") and i + 1 < n and re.match(r"^\s*\|[\s:|-]+\|\s*$", lines[i + 1]):
            para(buf); buf = []
            def cells(r):
                return [c.strip() for c in r.strip().strip("|").split("|")]
            head = cells(lines[i]); i += 2
            rows = []
            while i < n and lines[i].strip().startswith("|"):
                rows.append(cells(lines[i])); i += 1
            t = ["<div class=\"table\"><table><thead><tr>"] + ["<th>%s</th>" % inline(c) for c in head] + ["</tr></thead><tbody>"]
            for r in rows:
                t += ["<tr>"] + ["<td>%s</td>" % inline(c) for c in r] + ["</tr>"]
            out.append("".join(t) + "</tbody></table></div>")
            continue
        m = LIST_ITEM.match(line)
        if m:   # a list may start right under a paragraph line (as Markdown allows)
            para(buf); buf = []
            base = len(m.group(1))
            ordered = m.group(2)[0].isdigit()
            items = []
            while i < n:
                m = LIST_ITEM.match(lines[i])
                if m and len(m.group(1)) == base:
                    content_ind = len(m.group(1)) + len(m.group(2)) + 1
                    item = [m.group(3)]
                    i += 1
                    while i < n:
                        l = lines[i]
                        if not l.strip():
                            # a blank line: the item goes on if the next line is indented into it
                            if i + 1 < n and (len(lines[i + 1]) - len(lines[i + 1].lstrip())) > base and lines[i + 1].strip():
                                item.append(""); i += 1; continue
                            break
                        ind = len(l) - len(l.lstrip())
                        if ind <= base and (LIST_ITEM.match(l) or ind < base or not item):
                            break
                        if ind <= base and not LIST_ITEM.match(l):
                            # a lazy continuation of the item's paragraph
                            item.append(" " * content_ind + l.strip()); i += 1; continue
                        item.append(l[min(ind, content_ind):] if ind >= content_ind else l.lstrip()); i += 1
                    items.append(item)
                    continue
                break
            tag = "ol" if ordered else "ul"
            lis = []
            for it in items:
                body = blocks(it)
                # an item of one paragraph: no <p> round it
                if body.count("<p>") == 1 and body.startswith("<p>") and body.endswith("</p>"):
                    body = body[3:-4]
                elif body.startswith("<p>"):
                    body = body[3:].replace("</p>", "", 1)
                lis.append("<li>%s</li>" % body)
            out.append("<%s>%s</%s>" % (tag, "".join(lis), tag))
            continue
        buf.append(line)
        i += 1
    para(buf)
    return "\n".join(out)


def markdown(text):
    return blocks(text.replace("\t", "    ").split("\n"))


# ---- the pages ----------------------------------------------------------------------------------

def fingerprint(name):
    """a file's name carries its content's hash (style.<hash>.css, copied so by assets()): browsers
    keep style.css for hours (Cloudflare's browser cache), and a changed file then has a new address
    they fetch at once. In the name, not a query (?v=): Cloudflare's cache answered style.css?v=new
    with the old style.css right after a push (the headline's colours missing, 2026-10-15)"""
    stem, ext = os.path.splitext(name)
    return "%s.%s%s" % (stem, hashlib.sha256(open(os.path.join(SITE, name), "rb").read()).hexdigest()[:10], ext)


def page(title, body, depth, active="", desc=""):
    up = "../" * depth
    nav = [('index.html', "Home", "home"), ("games.html", "Games", "games"), ("manual/index.html", "Manual", "manual"), ("download.html", "Download", "download"), (REPO, "GitHub", "github")]
    links = "".join('<a href="%s"%s>%s</a>' % (h if h.startswith("http") else up + h, ' class="on"' if k == active else "", t) for h, t, k in nav)
    return """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>%s</title>
<meta name="description" content="%s">
<link rel="stylesheet" href="%s%s">
<link rel="icon" href="%s%s" type="image/svg+xml">
</head>
<body>
<header class="bar"><div class="wrap"><a class="brand" href="%sindex.html"><img src="%sicon.svg" alt="" width="28" height="28"><span>PSP<b>Exp</b></span></a><nav>%s</nav></div></header>
%s
<footer><div class="wrap">
<p>PSPExp is open source under the <a href="%s/blob/main/LICENSE">MIT licence</a>; games belong to their authors, under their own terms.
PSPExp is not made by or affiliated with Sony; "PSP" names the console it runs on.</p>
<p><a href="mailto:info@pspexp.dev">info@pspexp.dev</a> · <a href="%s">GitHub</a> · <a href="%s/blob/main/CHANGELOG.md">Release notes</a> · <a href="https://pkg.pspexp.dev/dists/stable/InRelease">Package repository</a></p>
</div></footer>
</body>
</html>
""" % (html.escape(title), html.escape(desc or "PSPExp: your PSP as the screen, sound and controller of a small Linux board, which runs the games."),
       up, fingerprint("style.css"), up, fingerprint("icon.svg"), up, up, links, body, REPO, REPO, REPO)


def manual_pages():
    os.makedirs(os.path.join(OUT, "manual"), exist_ok=True)
    side = "".join('<a href="%s">%s</a>' % (p, t) for _, p, t in MANUAL)
    for md, out, title in MANUAL:
        text = open(os.path.join(ROOT, "docs/manual", md), encoding="utf-8").read()
        body = markdown(text)
        h1 = re.search(r"<h1[^>]*>(.*?)</h1>", body)
        name = re.sub(r"<[^>]+>", "", h1.group(1)) if h1 else title
        nav = side.replace('<a href="%s">' % out, '<a class="on" href="%s">' % out)
        html_ = page("%s · PSPExp" % name, '<main class="wrap manual"><aside>%s</aside><article>%s</article></main>' % (nav, body), 1, "manual")
        open(os.path.join(OUT, "manual", out), "w", encoding="utf-8").write(html_)


def front_page():
    body = open(os.path.join(SITE, "index.html"), encoding="utf-8").read()
    video = ""
    if VIDEOS:
        video = ('<section class="wrap video"><h2>See it in action</h2><div class="videos">%s</div></section>' % "".join(
            '<figure><div class="frame"><iframe src="%s" title="PSPExp: %s" allow="encrypted-media; picture-in-picture" '
            'allowfullscreen loading="lazy"></iframe></div><figcaption>%s</figcaption></figure>' % (u, html.escape(t), html.escape(t))
            for u, t in VIDEOS))
    body = body.replace("{{REPO}}", REPO).replace("{{VIDEO}}", video)
    open(os.path.join(OUT, "index.html"), "w", encoding="utf-8").write(page("PSPExp: Experiment. Experience. Expand.", body, 0, "home"))


KINDS = {"PSP": r"^PSPExp-psp-.*\.zip$", "PI": r"^pspexp-pi-.*\.img\.xz$", "GS": r"^pspexp-gameshell-.*\.img\.xz$"}


def size(b):
    return "%.2f GB" % (b / 1e9) if b >= 1e9 else "%d MB" % round(b / 1e6) if b >= 1e6 else "%d KB" % max(1, round(b / 1e3))


def download_page():
    """the latest release's three files; when GitHub's API does not answer (a build offline, or its
    limit spent) the version and the files' addresses from CHANGELOG.md's newest release, the counts
    left to the page's script"""
    v = {"VERSION": "", "DATE": "", "TOTAL": "–", "TOTAL_UNIT": "downloads", "RELEASE_PAGE": RELEASES, "REPO": REPO}
    for k in KINDS:
        v.update({k + "_URL": RELEASES, k + "_SIZE": "", k + "_NAME": "", k + "_COUNT": "–", k + "_UNIT": "downloads", k + "_SHA": ""})
    try:
        hdr = {"Accept": "application/json", "User-Agent": "pspexp-site"}   # (application/vnd.github+json with per_page=100 got an empty list, 2026-10-22)
        if os.environ.get("GITHUB_TOKEN"):   # (Cloudflare's build machines share addresses: GitHub's 60 an hour ran out)
            hdr["Authorization"] = "Bearer " + os.environ["GITHUB_TOKEN"]
        req = urllib.request.Request(REPO.replace("github.com", "api.github.com/repos") + "/releases?per_page=100", headers=hdr)
        got = json.load(urllib.request.urlopen(req, timeout=20))
        rels = [r for r in got if not r["draft"] and not r["prerelease"]]
        print("download page: GitHub listed %d releases, %d published" % (len(got), len(rels)))
    except Exception as e:
        print("download page: no release information from GitHub (%s)" % e)
        rels = []
    if not rels:   # (whatever the reason: Cloudflare's build of 2026-10-22 still had no version with the fallback on failures only)
        m = re.search(r"^## (\d+\.\d+\.\d+) \((\d{4}-\d\d-\d\d)\)", open(os.path.join(ROOT, "CHANGELOG.md"), encoding="utf-8").read(), re.M)
        print("download page: the version from CHANGELOG.md: %s" % (m.group(1) if m else "none"))
        if m:   # the files' own addresses, which need no API: their names follow the release's
            ver, d = m.group(1), datetime.date.fromisoformat(m.group(2))
            base = "%s/releases/download/v%s/" % (REPO, ver)
            names = {"PSP": "PSPExp-psp-v%s.zip", "PI": "pspexp-pi-v%s.img.xz", "GS": "pspexp-gameshell-v%s.img.xz"}
            v.update({"VERSION": ver, "DATE": "%d %s" % (d.day, d.strftime("%B %Y")), "RELEASE_PAGE": "%s/releases/tag/v%s" % (REPO, ver)})
            for k, n in names.items():
                v.update({k + "_URL": base + n % ver, k + "_NAME": n % ver})
    if rels:
        latest, total = rels[0], 0
        d = datetime.date.fromisoformat(latest["published_at"][:10])
        v.update({"VERSION": latest["tag_name"].lstrip("v"), "DATE": "%d %s" % (d.day, d.strftime("%B %Y")),
                  "RELEASE_PAGE": latest["html_url"]})
        for k, pat in KINDS.items():
            n = sum(a["download_count"] for r in rels for a in r["assets"] if re.match(pat, a["name"]))
            total += n
            v[k + "_COUNT"], v[k + "_UNIT"] = "{:,}".format(n), "download" if n == 1 else "downloads"
            for a in latest["assets"]:
                if re.match(pat, a["name"]):
                    v.update({k + "_URL": a["browser_download_url"], k + "_SIZE": size(a["size"]), k + "_NAME": a["name"],
                              k + "_SHA": (a.get("digest") or "").replace("sha256:", "")})
        v["TOTAL"], v["TOTAL_UNIT"] = "{:,}".format(total), "download" if total == 1 else "downloads"
    body = open(os.path.join(SITE, "download.html"), encoding="utf-8").read()
    for k, x in v.items():
        body = body.replace("{{%s}}" % k, html.escape(x))
    open(os.path.join(OUT, "download.html"), "w", encoding="utf-8").write(
        page("Download PSPExp", body, 0, "download", "Download PSPExp: the PSP's part and the card images for the Raspberry Pi and the ClockworkPi GameShell."))


# ---- the games page: what Packages offers, read from recipes/ -------------------------------------

BOARDS = [("pi", "Raspberry Pi"), ("gameshell", "GameShell")]   # (the Omega2+ is not shown here: its 12 are all on a Pi too)
SYSTEM_ART = os.path.join(ROOT, "tools/pspexp-term/assets/systems")
EXTRA_ART = {"freedoom": "scripts/packages/art/freedoom-phase1.png"}


def read_recipe(path):
    """sections -> {key: value} (a repeated key: the last), as pspexp-pkg's rget reads them"""
    secs, sec = {}, None
    for line in open(path, encoding="utf-8"):
        t = line.strip()
        if not t or t.startswith("#"):
            continue
        if t.startswith("[") and t.endswith("]"):
            sec = t[1:-1]; secs.setdefault(sec, {}); continue
        if "=" in t and sec is not None:
            k, v = t.split("=", 1)
            secs[sec][k.strip()] = v.strip()
    return secs


def rget(secs, sec, key, board):
    """[SECTION:board] over [SECTION], as on the board"""
    ov = secs.get("%s:%s" % (sec, board), {})
    return ov[key] if key in ov else secs.get(sec, {}).get(key, "")


def recipes():
    out = []
    root = os.path.join(ROOT, "recipes")
    for rid in sorted(os.listdir(root)):
        path = os.path.join(root, rid, "recipe")
        if not os.path.isfile(path):
            continue
        r = read_recipe(path)
        fam = lambda b: "omega" if b == "omega" else "debian"
        boards = [b for b, _ in BOARDS if rget(r, fam(b), "apt", b) or rget(r, fam(b), "build", b)]
        cover = None
        if os.path.isfile(os.path.join(root, rid, "cover.png")):
            cover = os.path.join(root, rid, "cover.png")
        elif r.get("system", {}).get("thumbs") and os.path.isfile(os.path.join(SYSTEM_ART, r["system"]["thumbs"] + ".png")):
            cover = os.path.join(SYSTEM_ART, r["system"]["thumbs"] + ".png")
        elif rid in EXTRA_ART:
            cover = os.path.join(ROOT, EXTRA_ART[rid])
        g = r.get("entry", {}).get("graphics") or r.get("system", {}).get("graphics", "")
        out.append({"id": rid, "title": rget(r, "package", "title", "pi"), "desc": rget(r, "package", "description", "pi"),
                    "category": r.get("package", {}).get("category", "game"), "terms": bool(r.get("package", {}).get("terms")),
                    "boards": boards, "graphics": g, "cover": cover, "warn": rget(r, "debian", "warn", "gameshell")})
    return out


def games_page():
    items = [x for x in recipes() if x["boards"]]
    os.makedirs(os.path.join(OUT, "covers"), exist_ok=True)

    def card(x):
        if x["cover"]:
            name = x["id"] + os.path.splitext(x["cover"])[1]
            shutil.copy(x["cover"], os.path.join(OUT, "covers", name))
            art = '<img src="covers/%s" alt="" loading="lazy" width="96" height="96"%s>' % (html.escape(name), ' class="sys"' if x["category"] == "system" else "")
        else:
            art = '<span>%s</span>' % html.escape(x["title"])
        tags = "".join('<span class="tag board">%s</span>' % n for b, n in BOARDS if b in x["boards"])
        notes = []
        if re.search(r"your own|own copy", x["desc"], re.I):
            notes.append("your own copy")
        if x["terms"]:
            notes.append("terms to accept")
        if x["graphics"] == "board-gpu":
            notes.append("drawn by the Pi's GPU")
        elif x["graphics"] == "opengl":
            notes.append("drawn by the PSP's GPU")
        tags += "".join('<span class="tag">%s</span>' % html.escape(n) for n in notes)
        return ('<article class="pkg" data-boards="%s" data-text="%s"><div class="pkg-art">%s</div><div class="pkg-body"><h3>%s</h3><p>%s</p><div class="tags">%s</div></div></article>'
                % (" ".join(x["boards"]), html.escape((x["title"] + " " + x["desc"]).lower()), art, html.escape(x["title"]), html.escape(x["desc"]), tags))

    games = [x for x in items if x["category"] != "system"]
    systems = [x for x in items if x["category"] == "system"]
    count = {b: sum(1 for x in items if b in x["boards"]) for b, _ in BOARDS}
    chips = '<button class="chip on" data-board="">All <b>%d</b></button>' % len(items) + "".join(
        '<button class="chip" data-board="%s">%s <b>%d</b></button>' % (b, n, count[b]) for b, n in BOARDS)
    body = """<main class="wrap games">
<section class="games-hero">
  <h1>Games and emulators</h1>
  <p class="lead">Everything here installs from the PSP itself: <b>Apps &gt; Packages</b>, then cross. %d games and %d emulated systems; the board does the downloading. Some need your own copy of the game, and a few are drawn by the Raspberry Pi's graphics chip, so they are on a Pi only.</p>
  <div class="filters">%s<input type="search" placeholder="Search" aria-label="Search the games"></div>
</section>
<h2>Games</h2>
<div class="pkg-grid">%s</div>
<h2>Emulators</h2>
<p class="sub">Bring your own ROMs: copy them to the system's folder (Apps &gt; Files).</p>
<div class="pkg-grid">%s</div>
<p class="credits">The list is the packages' recipes in <a href="%s/tree/main/recipes">the repository</a>, as the boards read them. Pictures: the games' own, see <a href="%s/blob/main/scripts/packages/art/README.md">their sources and licences</a>; the systems' from the <a href="https://github.com/libretro/retroarch-assets">RetroArch assets</a> (CC BY 4.0).</p>
</main>
<script>
(function () {
  var board = "", text = "", chips = document.querySelectorAll(".chip"), box = document.querySelector(".filters input");
  function apply() {
    document.querySelectorAll(".pkg").forEach(function (c) {
      var ok = (!board || c.dataset.boards.split(" ").indexOf(board) >= 0) && (!text || c.dataset.text.indexOf(text) >= 0);
      c.style.display = ok ? "" : "none";
    });
  }
  chips.forEach(function (b) { b.addEventListener("click", function () { chips.forEach(function (x) { x.classList.remove("on"); }); b.classList.add("on"); board = b.dataset.board; apply(); }); });
  box.addEventListener("input", function () { text = box.value.trim().toLowerCase(); apply(); });
})();
</script>""" % (len(games), len(systems), chips, "".join(card(x) for x in games), "".join(card(x) for x in systems), REPO, REPO)
    open(os.path.join(OUT, "games.html"), "w", encoding="utf-8").write(
        page("Games · PSPExp", body, 0, "games", "The games and emulators PSPExp installs from the PSP: what runs on a Raspberry Pi or a ClockworkPi GameShell."))
    print("games page: %d games, %d systems (%s)" % (len(games), len(systems), ", ".join("%s %d" % (n, count[b]) for b, n in BOARDS)))


def assets():
    for f in ("style.css", "icon.svg"):
        shutil.copy(os.path.join(SITE, f), OUT)
        shutil.copy(os.path.join(SITE, f), os.path.join(OUT, fingerprint(f)))
    os.makedirs(os.path.join(OUT, "images"), exist_ok=True)
    for f in os.listdir(os.path.join(ROOT, "docs/images")):
        shutil.copy(os.path.join(ROOT, "docs/images", f), os.path.join(OUT, "images", f))
    os.makedirs(os.path.join(OUT, "fonts"), exist_ok=True)
    for f in ("Inter-Regular.ttf", "Inter-SemiBold.ttf", "Inter-LICENSE.txt"):
        shutil.copy(os.path.join(ROOT, "tools/pspexp-term/assets", f), os.path.join(OUT, "fonts", f))


def install_script():
    """install.sh: PSPExp on an existing Raspberry Pi OS in one command (curl -fsSL https://pspexp.dev/install.sh | sh),
    made from site/install.sh with the public half of the repository's signing key (the armoured key's body, which is the binary
    keyring apt and gpgv read: no gpg needed here) and the card image's own board-files.sh"""
    asc = open(os.path.join(ROOT, "scripts/packages/pspexp-archive-key.asc"), encoding="ascii").read().splitlines()
    body = [l for l in asc[asc.index("") + 1:] if l and not l.startswith("-----") and not l.startswith("=")]
    import base64, textwrap
    key = base64.b64decode("".join(body))   # (checked: it decodes; its CRC line is left out)
    board = open(os.path.join(ROOT, "scripts/pi/board-files.sh"), encoding="utf-8").read().rstrip("\n")
    for marker in ("KEY", "BOARD"):
        assert marker not in board.splitlines(), "board-files.sh has a line that would end install.sh's here-document"
    s = open(os.path.join(SITE, "install.sh"), encoding="utf-8").read()
    s = s.replace("{{KEYRING_B64}}", "\n".join(textwrap.wrap(base64.b64encode(key).decode(), 76))).replace("{{BOARD_FILES}}", board)
    open(os.path.join(OUT, "install.sh"), "w", encoding="utf-8").write(s)
    print("install.sh: %d bytes, the key %d bytes" % (len(s), len(key)))


def main():
    if os.path.exists(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    assets()
    front_page()
    games_page()
    download_page()
    install_script()
    manual_pages()
    # by address (Cloudflare Pages matches a rule's https://host/ against the request): the pages.dev
    # copy stays out of search engines so they list pspexp.dev alone; previews are noindex already
    hosts = HOSTS + ([] if LAUNCHED else SOON)
    open(os.path.join(OUT, "_headers"), "w").write(
        "".join("https://%s/*\n  X-Robots-Tag: noindex, nofollow\n" % h for h in hosts))
    n = sum(len(fs) for _, _, fs in os.walk(OUT))
    print("site/out: %d files" % n)


if __name__ == "__main__":
    main()
