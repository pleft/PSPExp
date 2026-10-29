/* Media's song titles from the files (tools/pspexp-term/tags.c): ID3v2.3, ID3v2.4 in UTF-8,
 * ID3v1, FLAC's and Ogg Opus's comments, and nothing for other bytes */
#include "test_util.h"
#include "../tools/pspexp-term/tags.h"
#include <string.h>

static size_t frame(uint8_t *p, const char *id, int enc, const char *text)
{
    size_t n = strlen(text) + 1;
    memcpy(p, id, 4);
    p[4] = 0; p[5] = 0; p[6] = (uint8_t)(n >> 8); p[7] = (uint8_t)n; p[8] = p[9] = 0;
    p[10] = (uint8_t)enc;
    memcpy(p + 11, text, n - 1);
    return 10 + n;
}

static size_t le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); return 4; }
static size_t comments(uint8_t *p, const char *a, const char *b)
{
    size_t i = 0;
    i += le(p + i, 4); memcpy(p + i, "test", 4); i += 4;
    i += le(p + i, 2);
    i += le(p + i, (uint32_t)strlen(a)); memcpy(p + i, a, strlen(a)); i += strlen(a);
    i += le(p + i, (uint32_t)strlen(b)); memcpy(p + i, b, strlen(b)); i += strlen(b);
    return i;
}

int main(void)
{
    uint8_t b[512], tail[128];
    char t[64], a[64];
    size_t n;
    /* ID3v2.3: TPE1 before TIT2, Latin-1 */
    memset(b, 0, sizeof(b));
    memcpy(b, "ID3\3\0\0", 6);
    n = 10;
    n += frame(b + n, "TPE1", 0, "Test Artist");
    n += frame(b + n, "TIT2", 0, "Track 1");
    b[9] = (uint8_t)(n - 10);
    CHECK_EQ(tags_parse(b, n + 20, NULL, t, sizeof(t), a, sizeof(a)), 1);
    CHECK(!strcmp(t, "Track 1"));
    CHECK(!strcmp(a, "Test Artist"));
    /* ID3v2.4, UTF-8 */
    memset(b, 0, sizeof(b));
    memcpy(b, "ID3\4\0\0", 6);
    n = 10 + frame(b + 10, "TIT2", 3, "Caf\xc3\xa9");
    b[9] = (uint8_t)(n - 10);
    CHECK_EQ(tags_parse(b, n, NULL, t, sizeof(t), a, sizeof(a)), 1);
    CHECK(!strcmp(t, "Caf\xc3\xa9"));
    CHECK(!strcmp(a, ""));
    /* ID3v1 only, its spaces trimmed */
    memset(b, 0xFF, 64);
    memset(tail, ' ', sizeof(tail));
    memcpy(tail, "TAG", 3); memcpy(tail + 3, "Old Song", 8); memcpy(tail + 33, "Old Band", 8);
    CHECK_EQ(tags_parse(b, 64, tail, t, sizeof(t), a, sizeof(a)), 1);
    CHECK(!strcmp(t, "Old Song"));
    CHECK(!strcmp(a, "Old Band"));
    /* FLAC: STREAMINFO, then VORBIS_COMMENT (the last) */
    memset(b, 0, sizeof(b));
    memcpy(b, "fLaC", 4);
    b[4] = 0; b[5] = 0; b[6] = 0; b[7] = 34;
    n = 8 + 34;
    {
        size_t c = comments(b + n + 4, "title=Flac Song", "ARTIST=Flac Band");
        b[n] = 0x84; b[n + 1] = 0; b[n + 2] = (uint8_t)(c >> 8); b[n + 3] = (uint8_t)c;
        n += 4 + c;
    }
    CHECK_EQ(tags_parse(b, n, NULL, t, sizeof(t), a, sizeof(a)), 1);
    CHECK(!strcmp(t, "Flac Song"));
    CHECK(!strcmp(a, "Flac Band"));
    /* Ogg Opus: OpusHead's page, then OpusTags */
    memset(b, 0, sizeof(b));
    memcpy(b, "OggS", 4);
    memcpy(b + 28, "OpusHead", 8);
    memcpy(b + 80, "OpusTags", 8);
    n = 88 + comments(b + 88, "TITLE=Opus Song", "ARTIST=Opus Band");
    CHECK_EQ(tags_parse(b, n, NULL, t, sizeof(t), a, sizeof(a)), 1);
    CHECK(!strcmp(t, "Opus Song"));
    CHECK(!strcmp(a, "Opus Band"));
    /* anything else */
    memset(b, 0x55, sizeof(b));
    CHECK_EQ(tags_parse(b, sizeof(b), NULL, t, sizeof(t), a, sizeof(a)), 0);
    CHECK(!strcmp(t, ""));
    return test_summary("tags");
}
