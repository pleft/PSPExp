/*
 * The capture plugin's pacing (psp/capture/pace.c) against games modelled on the PSP's
 * measurements (2026-10-25): in a scene a game runs at its own rate, and each frame copied a
 * second costs it `cost` frames a second (Gran Turismo's heavier stretch about 2: 20 copies left
 * it at 20 fps, 12 at 36, 4.5 at 51; Burnout Legends' races about 0.6, its copies 23-25 ms; most
 * scenes nothing). Checked: the game keeps its pace, clips get frames where copying is free, and
 * the seconds without copies (measuring) stay few.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../psp/capture/pace.c"

typedef struct { const char *name; double own, cost; int seconds; } Scene;

static int g_fail;
static void check(int ok, const char *what) { printf("  %s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) g_fail = 1; }

/* the game's frames in a second at a copy gap: the copies cannot outnumber its frames */
static double game_rate(const Scene *s, unsigned gap, double *copies)
{
    double want = gap == PACE_NONE ? 0 : 1e6 / gap, fps;
    if (want > 30.5) want = 30.5;   /* (the plugin's 30 a second at most) */
    fps = s->own - s->cost * want;
    if (fps < want) { fps = s->own / (1 + s->cost); want = fps; }   /* (every frame copied) */
    if (fps < 1) fps = 1;
    *copies = want;
    return fps;
}

/* one game: its scenes in turn; per scene, the game's mean rate against its own, the copies */
static void run(const char *game, const Scene *sc, int n, double min_ratio_costly, double min_clip_free, int max_pauses, int verbose)
{
    Pace p;
    unsigned gap = 30000;
    int i, t, pauses = 0;
    srand(7);
    pace_init(&p);
    printf("%s:\n", game);
    for (i = 0; i < n; i++) {
        double sum = 0, sumc = 0, steady = 0, steadyc = 0;
        int ns = 0;
        for (t = 0; t < sc[i].seconds; t++) {
            double copies, fps = game_rate(&sc[i], gap, &copies) + (rand() % 3 - 1);
            unsigned rate = fps < 0 ? 0 : (unsigned)(fps + 0.5);
            sum += fps; sumc += copies;
            if (t >= 20) { steady += fps; steadyc += copies; ns++; }   /* (after the pacing has settled) */
            if (gap == PACE_NONE) pauses++;
            gap = pace_second(&p, rate, copies > 0 || gap == PACE_NONE);
            if (p.said && verbose) { printf("      %3d s ", t); printf(pace_note(p.said), g_pace_per_s[p.level], p.a, p.b); printf("\n"); }
        }
        printf("    %-20s own %4.1f: the game %4.1f (settled %4.1f), copies %4.1f a second (settled %4.1f)\n", sc[i].name, sc[i].own,
               sum / sc[i].seconds, ns ? steady / ns : 0, sumc / sc[i].seconds, ns ? steadyc / ns : 0);
        if (ns && sc[i].cost > 0.1)   /* (as well as the fewest copies allow: 5 a second) */
            check(steady / ns >= (sc[i].own - sc[i].cost * 5) * min_ratio_costly, "a costly scene keeps the game's pace once settled (as 5 copies a second allow)");
        if (ns && sc[i].cost <= 0.1) check(steadyc / ns >= min_clip_free, "a free scene gets its clip frames once settled");
    }
    printf("    seconds without copies: %d\n", pauses);
    check(pauses <= max_pauses, "few seconds without copies");
}

int main(int argc, char **argv)
{
    int v = argc > 1;
    static const Scene gt[] = { { "normal", 60, 0, 60 }, { "heavy stretch", 60, 2.1, 40 }, { "normal again", 60, 0, 60 } };
    /* (the PSP, 2026-10-25: a hitch of a second as the heavy stretch began, measured as the game's own
     * rate, 18 fps without copies against 53 with them: its stretch then left at 43 instead of 50) */
    static const Scene gt_hitch[] = { { "normal", 60, 0, 30 }, { "heavier", 60, 0.6, 4 }, { "hitch", 18, 0, 2 },
                                      { "heavy stretch", 60, 2.1, 40 }, { "normal again", 60, 0, 60 } };
    static const Scene burnout[] = { { "menu", 60, 0, 20 }, { "load", 10, 0, 5 }, { "race", 30, 0.6, 70 },
                                     { "load", 10, 0, 5 }, { "menu", 60, 0, 25 }, { "race", 30, 0.6, 70 } };
    static const Scene bomberman[] = { { "play", 30, 0, 90 } };
    static const Scene ace[] = { { "menu", 60, 0, 25 }, { "mission", 30, 0.05, 90 } };
    run("Gran Turismo", gt, 3, 0.97, 25, 8, v);
    run("Gran Turismo, a hitch", gt_hitch, 5, 0.97, 25, 10, v);
    run("Burnout Legends", burnout, 6, 0.97, 25, 12, v);
    run("Bomberman", bomberman, 1, 0.97, 25, 2, v);
    run("Ace Combat X", ace, 2, 0.97, 25, 4, v);
    {   /* the PSP's sequence (Gran Turismo, 2026-10-25): at 60, a fall to 53 measured, the measured second a
         * hitch of 18 (slower than copying: not the game's own rate), then the heavy stretch */
        static const unsigned seq[] = { 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 53, 18 };
        Pace p;
        unsigned i;
        pace_init(&p);
        printf("a hitch in a measured second:\n");
        for (i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
            pace_second(&p, seq[i], 1);
            if (p.said && v) { printf("      %2u ", i); printf(pace_note(p.said), g_pace_per_s[p.level], p.a, p.b); printf("\n"); }
        }
        printf("    its rate without copies kept at %u\n", p.free_rate);
        check(p.free_rate >= 55, "a measured second slower than copying is not taken for the game's own rate");
    }
    printf(g_fail ? "FAILED\n" : "all passed\n");
    return g_fail;
}
