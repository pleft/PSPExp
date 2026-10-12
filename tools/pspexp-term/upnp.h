/*
 * Media servers on the network (UPnP / DLNA: MiniDLNA, Jellyfin, Plex, Kodi,
 * a NAS or a router), for Media's Browse.
 *
 * Found by SSDP (an M-SEARCH to 239.255.255.250:1900 for MediaServer:1);
 * each answer's description (an HTTP GET of its LOCATION) gives the
 * server's name and its ContentDirectory's control address; Browse (a SOAP
 * POST, BrowseDirectChildren) lists a folder: folders, and items with the
 * address ffmpeg plays them from. HTTP goes through curl (the boards have
 * it for the installers); the XML is read by the few tags that matter.
 * Calls block (discovery for its wait, a listing for one request): Media
 * runs them on a thread.
 */
#ifndef PSPEXP_TERM_UPNP_H
#define PSPEXP_TERM_UPNP_H

#include <stddef.h>
#include <stdint.h>

typedef struct UpnpServer {
    char name[128];           /* friendlyName: "MiniDLNA on nas" */
    char location[512];       /* its description */
    char control[512];        /* the ContentDirectory's controlURL, absolute */
} UpnpServer;

enum { UPNP_FOLDER, UPNP_AUDIO, UPNP_VIDEO, UPNP_IMAGE, UPNP_OTHER };

typedef struct UpnpEntry {
    char id[256];             /* the object's id (a folder: what to browse next) */
    char title[256];
    char url[1024];           /* an item's resource (http://...), "" for a folder */
    int  kind;                /* UPNP_* */
    double dur;               /* seconds, 0 when not given */
} UpnpEntry;

/* the media servers answering within wait_ms (at most max, each once, by name); how many */
int  upnp_discover(UpnpServer *out, int max, int wait_ms);

/* the children of folder id ("0": the server's top): *out malloc'd (free it),
 * *n of them; 0, or -1 with the reason in err */
int  upnp_browse(const UpnpServer *s, const char *id, UpnpEntry **out, int *n, char *err, size_t en);

/* for tests: a description's name and control address (base: its own URL) */
int  upnp_parse_description(const char *xml, const char *base, UpnpServer *out);
/* a Browse answer (the SOAP envelope, its DIDL-Lite escaped inside) */
int  upnp_parse_browse(const char *soap, UpnpEntry **out, int *n);

#endif
