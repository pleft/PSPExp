/*
 * Media servers (tools/pspexp-term/upnp.c): a description read for its name
 * and ContentDirectory address (relative, absolute, with URLBase), and a
 * Browse answer for its folders and items, as MiniDLNA writes them.
 */
#include "test_util.h"
#include "../tools/pspexp-term/upnp.h"
#include <stdlib.h>
#include <string.h>

static const char DESC[] =
    "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><specVersion><major>1</major></specVersion>"
    "<device><deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType><friendlyName>nas: minidlna</friendlyName>"
    "<serviceList><service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>"
    "<controlURL>/ctl/ConnectionMgr</controlURL></service>"
    "<service><serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType>"
    "<controlURL>/ctl/ContentDir</controlURL></service></serviceList></device></root>";

static const char BROWSE[] =
    "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
    "<u:BrowseResponse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\"><Result>"
    "&lt;DIDL-Lite xmlns:dc=&quot;http://purl.org/dc/elements/1.1/&quot; xmlns:upnp=&quot;urn:schemas-upnp-org:metadata-1-0/upnp/&quot;&gt;"
    "&lt;container id=&quot;64$0&quot; parentID=&quot;64&quot; restricted=&quot;1&quot; childCount=&quot;3&quot;&gt;"
    "&lt;dc:title&gt;Films &amp;amp; series&lt;/dc:title&gt;&lt;upnp:class&gt;object.container.storageFolder&lt;/upnp:class&gt;&lt;/container&gt;"
    "&lt;item id=&quot;64$1&quot; parentID=&quot;64&quot; restricted=&quot;1&quot;&gt;&lt;dc:title&gt;Big Buck Bunny&lt;/dc:title&gt;"
    "&lt;upnp:class&gt;object.item.videoItem&lt;/upnp:class&gt;"
    "&lt;res size=&quot;1000&quot; duration=&quot;0:09:56.458&quot; protocolInfo=&quot;http-get:*:video/mp4:*&quot;&gt;http://192.0.2.5:8200/MediaItems/22.mp4&lt;/res&gt;&lt;/item&gt;"
    "&lt;item id=&quot;64$2&quot; parentID=&quot;64&quot;&gt;&lt;dc:title&gt;Song&lt;/dc:title&gt;&lt;upnp:class&gt;object.item.audioItem.musicTrack&lt;/upnp:class&gt;"
    "&lt;res protocolInfo=&quot;http-get:*:audio/mpeg:*&quot;&gt;http://192.0.2.5:8200/MediaItems/7.mp3?a=1&amp;amp;b=2&lt;/res&gt;&lt;/item&gt;"
    "&lt;item id=&quot;64$3&quot;&gt;&lt;dc:title&gt;No resource&lt;/dc:title&gt;&lt;upnp:class&gt;object.item.videoItem&lt;/upnp:class&gt;&lt;/item&gt;"
    "&lt;/DIDL-Lite&gt;</Result><NumberReturned>3</NumberReturned><TotalMatches>3</TotalMatches><UpdateID>1</UpdateID>"
    "</u:BrowseResponse></s:Body></s:Envelope>";

int main(void)
{
    UpnpServer s;
    UpnpEntry *e;
    int n;

    CHECK_EQ(upnp_parse_description(DESC, "http://192.0.2.5:8200/rootDesc.xml", &s), 0);
    CHECK(!strcmp(s.name, "nas: minidlna"));
    CHECK(!strcmp(s.control, "http://192.0.2.5:8200/ctl/ContentDir"));   /* the ContentDirectory's, not the first service's */
    {   /* a control address relative to the description, and one under URLBase */
        static const char REL[] = "<root><device><friendlyName>Kodi</friendlyName><serviceList><service><serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType>"
                                  "<controlURL>cd/control</controlURL></service></serviceList></device></root>";
        static const char BASE[] = "<root><URLBase>http://192.0.2.9:1400/</URLBase><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType>"
                                   "<controlURL>/cd</controlURL></service></serviceList></device></root>";
        CHECK_EQ(upnp_parse_description(REL, "http://192.0.2.7:1234/upnp/desc.xml", &s), 0);
        CHECK(!strcmp(s.control, "http://192.0.2.7:1234/upnp/cd/control"));
        CHECK_EQ(upnp_parse_description(BASE, "http://192.0.2.9:1400/xml/desc.xml", &s), 0);
        CHECK(!strcmp(s.control, "http://192.0.2.9:1400/cd") && !strcmp(s.name, "Media server"));
        CHECK(upnp_parse_description("<root><device><friendlyName>printer</friendlyName></device></root>", "http://x/", &s) < 0);
    }

    CHECK_EQ(upnp_parse_browse(BROWSE, &e, &n), 0);
    CHECK_EQ(n, 3);   /* the item without a resource left out */
    if (n == 3) {
        CHECK(e[0].kind == UPNP_FOLDER && !strcmp(e[0].id, "64$0") && !strcmp(e[0].title, "Films & series") && !e[0].url[0]);
        CHECK(e[1].kind == UPNP_VIDEO && !strcmp(e[1].title, "Big Buck Bunny") && !strcmp(e[1].url, "http://192.0.2.5:8200/MediaItems/22.mp4"));
        CHECK(e[1].dur > 596.4 && e[1].dur < 596.5);
        CHECK(e[2].kind == UPNP_AUDIO && !strcmp(e[2].url, "http://192.0.2.5:8200/MediaItems/7.mp3?a=1&b=2"));
    }
    free(e);
    CHECK(upnp_parse_browse("<html>not soap</html>", &e, &n) < 0);
    return test_summary("upnp");
}
