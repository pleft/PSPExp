/*
 * The launcher's board settings (tools/pspexp-term/launcher_board.c): the
 * zone table, both Wi-Fi scan formats (NetworkManager's terse lines, Onion's
 * ubus JSON), and the commands, quoted.
 */
#include "test_util.h"
#include "../tools/pspexp-term/launcher_board.h"
#include <string.h>

int main(void)
{
    BoardNet nets[16];
    char cmd[600];
    int n, i;

    /* zones: every entry has both forms; lookups */
    for (i = 0; i < board_zone_count(); i++) {
        const BoardZone *z = board_zone(i);
        CHECK(z->iana[0] && z->posix[0] && z->label[0] && z->offset[0]);
    }
    CHECK(board_zone_find("Europe/Berlin") >= 0);
    CHECK(!strcmp(board_zone(board_zone_find("Europe/Berlin"))->posix, "CET-1CEST,M3.5.0,M10.5.0/3"));
    CHECK_EQ(board_zone_find("Mars/Olympus"), -1);

    /* NetworkManager: escaped colons, the same name twice (strongest kept),
     * hidden networks left out, open ones need no password */
    n = board_wifi_parse(BOARD_WIFI_NM,
        "Home\\:Net:62:WPA2\n"
        "Cafe:80:\n"
        ":90:WPA2\n"
        "Home\\:Net:71:WPA2\n"
        "Neighbour:20:WPA1 WPA2\n"
        "S\t0c1d-uuid\tHome:Net\n"         /* saved: its profile */
        "S\t77aa-uuid\tFar Away\n", nets, 16);   /* saved, out of range: not listed */
    CHECK_EQ(n, 3);
    if (n == 3) {
        CHECK(!strcmp(nets[0].ssid, "Cafe") && nets[0].signal == 80 && !nets[0].secure && !nets[0].saved);
        CHECK(!strcmp(nets[1].ssid, "Home:Net") && nets[1].signal == 71 && nets[1].secure);
        CHECK(nets[1].saved && !strcmp(nets[1].id, "0c1d-uuid"));
        CHECK(!strcmp(nets[2].ssid, "Neighbour") && !nets[2].saved);
    }

    /* Onion's wifi-scan: JSON objects in "results" */
    n = board_wifi_parse(BOARD_WIFI_ONION,
        "{\n\t\"results\": [\n"
        "\t\t{ \"channel\": \"4\", \"ssid\": \"Far\", \"encryption\": \"psk2\", \"signalStrength\": \"0\", \"rssi\": \"-98\" },\n"
        "\t\t{ \"channel\": \"6\", \"ssid\": \"Home \\\"5\\\"\", \"encryption\": \"psk2\", \"signalStrength\": \"100\" },\n"
        "\t\t{ \"channel\": \"11\", \"ssid\": \"Open\", \"encryption\": \"none\", \"signalStrength\": \"40\" }\n"
        "\t]\n}\n"
        "S\t-\tHome \"5\"\n", nets, 16);
    CHECK_EQ(n, 3);
    if (n == 3) {
        CHECK(!strcmp(nets[0].ssid, "Home \"5\"") && nets[0].secure && !strcmp(nets[0].encr, "psk2"));
        CHECK(nets[0].saved && !nets[0].id[0] && !nets[1].saved);
        CHECK(!strcmp(nets[1].ssid, "Open") && !nets[1].secure);
        CHECK(!strcmp(nets[2].ssid, "Far"));
    }

    /* commands: the name quoted for the shell, the password never in the text */
    {
        BoardNet net;
        memset(&net, 0, sizeof(net));
        snprintf(net.ssid, sizeof(net.ssid), "Bob's Wi-Fi");
        net.secure = 1;
        snprintf(net.encr, sizeof(net.encr), "psk2");
        board_wifi_connect_command(BOARD_WIFI_NM, &net, 1, cmd, sizeof(cmd));
        CHECK(strstr(cmd, "'Bob'\\''s Wi-Fi'") != NULL);
        CHECK(strstr(cmd, "\"$PSPEXP_WIFI_PSK\"") != NULL);
        /* on nmcli's standard input (--ask), from the shell's printf: never among its arguments */
        CHECK(!strncmp(cmd, "printf '%s\\n' \"$PSPEXP_WIFI_PSK\" | ", 34) && strstr(cmd, "nmcli --ask dev wifi connect") != NULL);
        CHECK(!strstr(cmd, " password "));
        CHECK(strstr(cmd, "|| { ") != NULL && strstr(cmd, "connection delete id 'Bob'\\''s Wi-Fi'") != NULL);
        board_wifi_connect_command(BOARD_WIFI_ONION, &net, 1, cmd, sizeof(cmd));
        CHECK(strstr(cmd, "wifisetup add -ssid 'Bob'\\''s Wi-Fi' -encr psk2") != NULL);
        CHECK(strstr(cmd, "priority -ssid 'Bob'\\''s Wi-Fi' -move top") != NULL);
        /* saved: joined with what the board keeps; a new password keeps the profile */
        net.saved = 1;
        snprintf(net.id, sizeof(net.id), "0c1d-uuid");
        board_wifi_connect_command(BOARD_WIFI_NM, &net, 0, cmd, sizeof(cmd));
        CHECK(strstr(cmd, "nmcli connection up uuid '0c1d-uuid'") != NULL && !strstr(cmd, "PSK"));
        board_wifi_connect_command(BOARD_WIFI_NM, &net, 1, cmd, sizeof(cmd));
        CHECK(strstr(cmd, "\"$PSPEXP_WIFI_PSK\"") != NULL && !strstr(cmd, "delete"));
        CHECK(strstr(cmd, "nmcli --ask dev wifi connect") != NULL && !strstr(cmd, " password "));
        board_wifi_connect_command(BOARD_WIFI_ONION, &net, 0, cmd, sizeof(cmd));
        CHECK(!strstr(cmd, "add") && !strstr(cmd, "PSK") && strstr(cmd, "wifisetup priority -ssid 'Bob'\\''s Wi-Fi' -move top") != NULL);
    }
    return test_summary("launcher_board");
}
