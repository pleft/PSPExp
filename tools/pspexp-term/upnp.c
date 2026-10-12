/* Media servers on the network: see upnp.h */
#include "upnp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---- the XML, by its tags ------------------------------------------------------------ */

/* "&lt;" and the others, in place */
static void unescape(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '&') {
            const char *e = strchr(s, ';');
            if (e && e - s < 10) {
                if (!strncmp(s, "&lt;", 4)) { *o++ = '<'; s += 4; continue; }
                if (!strncmp(s, "&gt;", 4)) { *o++ = '>'; s += 4; continue; }
                if (!strncmp(s, "&amp;", 5)) { *o++ = '&'; s += 5; continue; }
                if (!strncmp(s, "&quot;", 6)) { *o++ = '"'; s += 6; continue; }
                if (!strncmp(s, "&apos;", 6)) { *o++ = '\''; s += 6; continue; }
                if (s[1] == '#') {
                    unsigned long cp = s[2] == 'x' ? strtoul(s + 3, NULL, 16) : strtoul(s + 2, NULL, 10);
                    if (cp < 0x80) *o++ = (char)cp;
                    else if (cp < 0x800) { *o++ = (char)(0xC0 | cp >> 6); *o++ = (char)(0x80 | (cp & 63)); }
                    else if (cp < 0x10000) { *o++ = (char)(0xE0 | cp >> 12); *o++ = (char)(0x80 | ((cp >> 6) & 63)); *o++ = (char)(0x80 | (cp & 63)); }
                    s = (char *)e + 1;
                    continue;
                }
            }
        }
        *o++ = *s++;
    }
    *o = 0;
}

/* the next element named name (any namespace prefix: "dc:title" matches "title") from p:
 * where its text starts (*text) and ends (*end); its start tag (for attributes) in *tag.
 * NULL when there is none before limit */
static const char *element(const char *p, const char *limit, const char *name, const char **tag, const char **end)
{
    size_t nl = strlen(name);
    while ((p = strchr(p, '<')) && (!limit || p < limit)) {
        const char *n = p + 1, *c, *gt;
        if (*n == '/' || *n == '?' || *n == '!') { p++; continue; }
        c = n;
        while (*c && *c != ' ' && *c != '>' && *c != '/' && *c != '\t' && *c != '\n' && *c != '\r') c++;
        {   /* the local name: after the prefix */
            const char *colon = memchr(n, ':', (size_t)(c - n)), *local = colon ? colon + 1 : n;
            if ((size_t)(c - local) == nl && !strncmp(local, name, nl)) {
                if (!(gt = strchr(c, '>'))) return NULL;
                if (tag) *tag = p;
                if (gt[-1] == '/') { if (end) *end = gt + 1; return gt + 1; }   /* <empty/> */
                {   /* the end: </prefix:name> */
                    char close[96];
                    const char *e;
                    snprintf(close, sizeof(close), "</%.*s>", (int)(c - n), n);
                    e = strstr(gt + 1, close);
                    if (end) *end = e ? e : gt + 1;
                }
                return gt + 1;
            }
        }
        p++;
    }
    return NULL;
}

/* the text of the first <name> between p and limit, unescaped, into out */
static int text_of(const char *p, const char *limit, const char *name, char *out, size_t n)
{
    const char *end, *t = element(p, limit, name, NULL, &end);
    out[0] = 0;
    if (!t || end < t) return -1;
    snprintf(out, n, "%.*s", (int)(end - t), t);
    unescape(out);
    {   /* trimmed */
        char *s = out, *e;
        while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
        if (s != out) memmove(out, s, strlen(s) + 1);
        e = out + strlen(out);
        while (e > out && (e[-1] == ' ' || e[-1] == '\n' || e[-1] == '\r' || e[-1] == '\t')) *--e = 0;
    }
    return 0;
}

/* attribute name="..." of a start tag */
static int attr_of(const char *tag, const char *name, char *out, size_t n)
{
    const char *gt = strchr(tag, '>'), *p = tag;
    size_t nl = strlen(name);
    out[0] = 0;
    while ((p = strstr(p, name)) && (!gt || p < gt)) {
        if ((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n') && p[nl] == '=' && (p[nl + 1] == '"' || p[nl + 1] == '\'')) {
            char q = p[nl + 1];
            const char *v = p + nl + 2, *e = strchr(v, q);
            if (!e) return -1;
            snprintf(out, n, "%.*s", (int)(e - v), v);
            unescape(out);
            return 0;
        }
        p += nl;
    }
    return -1;
}

/* a relative address against base ("http://host:port/desc.xml") */
static void absolute(const char *base, const char *rel, char *out, size_t n)
{
    const char *host_end;
    if (!strncasecmp(rel, "http://", 7) || !strncasecmp(rel, "https://", 8)) { snprintf(out, n, "%s", rel); return; }
    host_end = strstr(base, "://");
    host_end = host_end ? strchr(host_end + 3, '/') : NULL;
    if (rel[0] == '/') snprintf(out, n, "%.*s%s", (int)(host_end ? host_end - base : (long)strlen(base)), base, rel);
    else {
        const char *slash = strrchr(base, '/');
        snprintf(out, n, "%.*s/%s", (int)(slash && slash > base + 8 ? slash - base : (long)strlen(base)), base, rel);
    }
}

int upnp_parse_description(const char *xml, const char *base, UpnpServer *out)
{
    const char *p = xml, *svc, *end;
    char url_base[512] = "";
    memset(out, 0, sizeof(*out));
    snprintf(out->location, sizeof(out->location), "%s", base);
    if (text_of(xml, NULL, "URLBase", url_base, sizeof(url_base)) < 0 || !url_base[0]) snprintf(url_base, sizeof(url_base), "%s", base);
    text_of(xml, NULL, "friendlyName", out->name, sizeof(out->name));
    while ((svc = element(p, NULL, "service", NULL, &end))) {
        char type[200], ctl[512];
        if (text_of(svc, end, "serviceType", type, sizeof(type)) == 0 && strstr(type, ":ContentDirectory:")
            && text_of(svc, end, "controlURL", ctl, sizeof(ctl)) == 0 && ctl[0]) {
            absolute(url_base, ctl, out->control, sizeof(out->control));
            if (!out->name[0]) snprintf(out->name, sizeof(out->name), "Media server");
            return 0;
        }
        p = end > svc ? end : svc;
    }
    return -1;
}

static double hms(const char *s)
{
    int h = 0, m = 0;
    double sec = 0;
    if (sscanf(s, "%d:%d:%lf", &h, &m, &sec) == 3) return h * 3600.0 + m * 60.0 + sec;
    return 0;
}

int upnp_parse_browse(const char *soap, UpnpEntry **out, int *n)
{
    const char *end, *r = element(soap, NULL, "Result", NULL, &end), *p;
    char *didl;
    int cap = 0;
    *out = NULL; *n = 0;
    if (!r || end < r) return -1;
    if (!(didl = malloc((size_t)(end - r) + 1))) return -1;
    memcpy(didl, r, (size_t)(end - r)); didl[end - r] = 0;
    unescape(didl);   /* the DIDL-Lite, escaped inside the SOAP answer */
    p = didl;
    for (;;) {   /* containers and items, in the server's order */
        const char *ctag, *cend, *itag, *iend, *c = element(p, NULL, "container", &ctag, &cend), *i = element(p, NULL, "item", &itag, &iend);
        const char *tag, *body, *stop;
        UpnpEntry *e;
        char cls[128] = "";
        int folder;
        if (!c && !i) break;
        folder = c && (!i || ctag < itag);
        tag = folder ? ctag : itag; body = folder ? c : i; stop = folder ? cend : iend;
        p = stop > body ? stop : body;
        if (*n == cap) {
            UpnpEntry *g = realloc(*out, sizeof(UpnpEntry) * (size_t)(cap = cap ? cap * 2 : 64));
            if (!g) break;
            *out = g;
        }
        e = &(*out)[*n];
        memset(e, 0, sizeof(*e));
        attr_of(tag, "id", e->id, sizeof(e->id));
        text_of(body, stop, "title", e->title, sizeof(e->title));
        text_of(body, stop, "class", cls, sizeof(cls));
        if (folder) e->kind = UPNP_FOLDER;
        else {
            const char *rtag, *rend, *res = element(body, stop, "res", &rtag, &rend);
            char d[32];
            e->kind = strstr(cls, "videoItem") ? UPNP_VIDEO : strstr(cls, "audioItem") ? UPNP_AUDIO : strstr(cls, "imageItem") ? UPNP_IMAGE : UPNP_OTHER;
            if (res && rend >= res) {
                snprintf(e->url, sizeof(e->url), "%.*s", (int)(rend - res), res);
                unescape(e->url);
                if (attr_of(rtag, "duration", d, sizeof(d)) == 0) e->dur = hms(d);
            }
            if (!e->url[0]) continue;   /* nothing to play */
        }
        if (!e->title[0]) snprintf(e->title, sizeof(e->title), "%s", e->id);
        (*n)++;
    }
    free(didl);
    return 0;
}

/* ---- HTTP, through curl ---------------------------------------------------------------- */

/* curl's answer (argv's standard output), up to cap bytes: malloc'd text, or NULL */
static char *fetch(char *const argv[], size_t cap)
{
    int fds[2];
    size_t len = 0, room = 65536;
    char *buf = malloc(room);
    pid_t pid;
    ssize_t r;
    int st;
    if (!buf || pipe(fds) < 0) { free(buf); return NULL; }
    if ((pid = fork()) < 0) { close(fds[0]); close(fds[1]); free(buf); return NULL; }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR), i;
        dup2(fds[1], 1); dup2(nul, 2); dup2(nul, 0);
        for (i = 3; i < 1024; i++) close(i);   /* not the PSP link */
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    while ((r = read(fds[0], buf + len, room - 1 - len)) > 0) {
        len += (size_t)r;
        if (len + 1 >= room) {
            char *g;
            if (room >= cap) break;
            if (!(g = realloc(buf, room *= 2))) break;
            buf = g;
        }
    }
    buf[len] = 0;
    close(fds[0]);
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || !len) { free(buf); return NULL; }
    return buf;
}

int upnp_browse(const UpnpServer *s, const char *id, UpnpEntry **out, int *n, char *err, size_t en)
{
    enum { PAGE = 500, MAX = 5000 };
    int start = 0;
    *out = NULL; *n = 0;
    for (;;) {   /* page by page: a big folder comes in several answers */
        char body[2048], len_hdr[64], *ans;
        char eid[512], *o = eid;
        const char *q;
        UpnpEntry *page;
        int got, total = -1;
        for (q = id; *q && o < eid + sizeof(eid) - 7; q++) {   /* the id, escaped for the XML */
            if (*q == '&') { memcpy(o, "&amp;", 5); o += 5; } else if (*q == '<') { memcpy(o, "&lt;", 4); o += 4; } else *o++ = *q;
        }
        *o = 0;
        snprintf(body, sizeof(body),
                 "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                 "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
                 "<s:Body><u:Browse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
                 "<ObjectID>%s</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag><Filter>*</Filter>"
                 "<StartingIndex>%d</StartingIndex><RequestedCount>%d</RequestedCount><SortCriteria></SortCriteria>"
                 "</u:Browse></s:Body></s:Envelope>", eid, start, PAGE);
        snprintf(len_hdr, sizeof(len_hdr), "Content-Length: %zu", strlen(body));
        {
            char *argv[] = { "curl", "-s", "-f", "-m", "10", "-X", "POST", "-H", "Content-Type: text/xml; charset=\"utf-8\"",
                             "-H", "SOAPACTION: \"urn:schemas-upnp-org:service:ContentDirectory:1#Browse\"", "-H", len_hdr,
                             "--data-binary", body, (char *)s->control, NULL };
            ans = fetch(argv, 16u << 20);
        }
        if (!ans) { snprintf(err, en, "%s did not answer", s->name); free(*out); *out = NULL; *n = 0; return -1; }
        if (upnp_parse_browse(ans, &page, &got) < 0) { free(ans); snprintf(err, en, "%s's answer could not be read", s->name); free(*out); *out = NULL; *n = 0; return -1; }
        {
            char t[32];
            if (text_of(ans, NULL, "TotalMatches", t, sizeof(t)) == 0) total = atoi(t);
            if (text_of(ans, NULL, "NumberReturned", t, sizeof(t)) == 0) start += atoi(t); else start += got;
        }
        free(ans);
        if (got) {
            UpnpEntry *g = realloc(*out, sizeof(UpnpEntry) * (size_t)(*n + got));
            if (!g) { free(page); break; }
            *out = g;
            memcpy(*out + *n, page, sizeof(UpnpEntry) * (size_t)got);
            *n += got;
        }
        free(page);
        if (total < 0 || start >= total || *n >= MAX || got == 0) break;
    }
    return 0;
}

/* ---- SSDP ------------------------------------------------------------------------------ */

static double now_s(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }

int upnp_discover(UpnpServer *out, int max, int wait_ms)
{
    static const char MSEARCH[] =
        "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
        "ST: urn:schemas-upnp-org:device:MediaServer:1\r\n\r\n";
    struct sockaddr_in to;
    char locs[16][512];
    int nloc = 0, fd, i, n = 0;
    unsigned char ttl = 2;
    double end = now_s() + wait_ms / 1000.0;
    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) return 0;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET; to.sin_port = htons(1900); to.sin_addr.s_addr = inet_addr("239.255.255.250");
    for (i = 0; i < 2; i++) sendto(fd, MSEARCH, sizeof(MSEARCH) - 1, 0, (struct sockaddr *)&to, sizeof(to));   /* twice: UDP may lose one */
    while (nloc < 16) {
        fd_set r;
        struct timeval tv;
        double left = end - now_s();
        char buf[2048], *l;
        ssize_t got;
        if (left <= 0) break;
        tv.tv_sec = (long)left; tv.tv_usec = (long)((left - (long)left) * 1e6);
        FD_ZERO(&r); FD_SET(fd, &r);
        if (select(fd + 1, &r, NULL, NULL, &tv) <= 0) break;
        if ((got = recv(fd, buf, sizeof(buf) - 1, 0)) <= 0) continue;
        buf[got] = 0;
        for (l = buf; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
            if (!strncasecmp(l, "LOCATION:", 9)) {
                char loc[512], *e;
                const char *v = l + 9;
                int k, seen = 0;
                while (*v == ' ') v++;
                snprintf(loc, sizeof(loc), "%s", v);
                if ((e = strpbrk(loc, "\r\n"))) *e = 0;
                for (k = 0; k < nloc; k++) if (!strcmp(locs[k], loc)) seen = 1;
                if (!seen && loc[0]) snprintf(locs[nloc++], sizeof(locs[0]), "%s", loc);
            }
    }
    close(fd);
    for (i = 0; i < nloc && n < max; i++) {   /* each one's description: its name, its ContentDirectory */
        char *argv[] = { "curl", "-s", "-f", "-m", "4", locs[i], NULL }, *xml = fetch(argv, 1u << 20);
        int k, dup = 0;
        if (!xml) continue;
        if (upnp_parse_description(xml, locs[i], &out[n]) == 0) {
            for (k = 0; k < n; k++) if (!strcmp(out[k].control, out[n].control)) dup = 1;   /* one server, several addresses */
            if (!dup) n++;
        }
        free(xml);
    }
    {   /* by name: the same order every time (they answer in any) */
        int a, b;
        for (a = 1; a < n; a++)
            for (b = a; b > 0 && strcasecmp(out[b - 1].name, out[b].name) > 0; b--) { UpnpServer t = out[b]; out[b] = out[b - 1]; out[b - 1] = t; }
    }
    return n;
}
