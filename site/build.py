#!/usr/bin/env python3
"""
The web page at https://pspexp.dev (GOING_PUBLIC: required at launch), built into site/out/:
  python3 site/build.py
- index.html: the front page, from site/index.html (its {{...}} filled in here);
- manual/*.html: the user manual, from docs/manual/*.md, so it is written once (README.md becomes
  manual/index.html; links between its pages follow, links out of it go to GitHub);
- style.css, the screenshots (docs/images) and the launcher's font (Inter, OFL, beside its licence).
Python's standard library only: Cloudflare Pages runs this as the build command (output: site/out).
The Markdown converter knows what the manual uses: headings, paragraphs, lists (nested), tables,
fenced code, block quotes, rules, and inline code, bold, italics, links and images.
"""
import html, os, re, shutil, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SITE = os.path.join(ROOT, "site")
OUT = os.path.join(SITE, "out")
REPO = "https://github.com/pleft/PSPExp"
RELEASES = REPO + "/releases/latest"
VIDEO = "https://www.youtube-nocookie.com/embed/CQm_w-4Oqfw"   # the demo video (youtu.be/CQm_w-4Oqfw), unlisted until launch day
LAUNCHED = True    # until launch day: every page says "do not index" (Cloudflare Pages' _headers);
                   # then True, with pspexp.dev attached: pspexp.dev is indexed, pages.dev never is
HOSTS = ["pspexp.pages.dev", "www.pspexp.dev"]   # always "do not index": one address in search results
SOON = ["pspexp.dev"]                            # "do not index" until launch day

MANUAL = [("README.md", "index.html", "The manual"), ("psp.md", "psp.html", "The PSP"),
          ("raspberry-pi.md", "raspberry-pi.html", "Raspberry Pi"), ("gameshell.md", "gameshell.html", "GameShell"),
          ("omega2.md", "omega2.html", "Omega2+"), ("using-pspexp.md", "using-pspexp.html", "Using PSPExp")]


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

def page(title, body, depth, active="", desc=""):
    up = "../" * depth
    nav = [('index.html', "Home", "home"), ("manual/index.html", "Manual", "manual"), (RELEASES, "Download", "download"), (REPO, "GitHub", "github")]
    links = "".join('<a href="%s"%s>%s</a>' % (h if h.startswith("http") else up + h, ' class="on"' if k == active else "", t) for h, t, k in nav)
    return """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>%s</title>
<meta name="description" content="%s">
<link rel="stylesheet" href="%sstyle.css">
<link rel="icon" href="%sicon.svg" type="image/svg+xml">
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
       up, up, up, up, links, body, REPO, REPO, REPO)


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
    if VIDEO:
        video = ('<section class="wrap video"><h2>See it in action</h2><div class="frame"><iframe src="%s" title="PSPExp in action" '
                 'allow="encrypted-media; picture-in-picture" allowfullscreen loading="lazy"></iframe></div></section>' % VIDEO)
    body = body.replace("{{RELEASES}}", RELEASES).replace("{{REPO}}", REPO).replace("{{VIDEO}}", video)
    open(os.path.join(OUT, "index.html"), "w", encoding="utf-8").write(page("PSPExp: your PSP, playing games it never had", body, 0, "home"))


def assets():
    shutil.copy(os.path.join(SITE, "style.css"), OUT)
    shutil.copy(os.path.join(SITE, "icon.svg"), OUT)
    os.makedirs(os.path.join(OUT, "images"), exist_ok=True)
    for f in os.listdir(os.path.join(ROOT, "docs/images")):
        shutil.copy(os.path.join(ROOT, "docs/images", f), os.path.join(OUT, "images", f))
    os.makedirs(os.path.join(OUT, "fonts"), exist_ok=True)
    for f in ("Inter-Regular.ttf", "Inter-SemiBold.ttf", "Inter-LICENSE.txt"):
        shutil.copy(os.path.join(ROOT, "tools/pspexp-term/assets", f), os.path.join(OUT, "fonts", f))


def main():
    if os.path.exists(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    assets()
    front_page()
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
