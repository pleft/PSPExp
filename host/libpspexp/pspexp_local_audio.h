/* libpspexp's sound on the board's own output (pspexp_local_audio.c) */
#ifndef PSPEXP_LOCAL_AUDIO_H
#define PSPEXP_LOCAL_AUDIO_H

#include <stdint.h>
#include <sys/types.h>
#include <time.h>

typedef struct PspExpLocalAudio {
    int fd;                 /* pw-play's input, -1 none */
    pid_t pid;
    uint32_t rate;
    char node[128];         /* the PipeWire sink */
    int want;               /* the route says the board's output */
    time_t mtime; long size;   /* the route file as last read */
    uint64_t checked_us, failed_us;
    uint32_t dropped;
    /* the PSP's volume buttons, while the route is the board's: pspexp-bt volume */
    uint32_t vol_buttons; uint64_t vol_next_us; int vol_pending; pid_t vol_pid;
    int vol_fd;             /* the running pspexp-bt volume's answer */
    int vol_now;            /* the device's volume, as it answered; -1 not known */
    int vol_report;         /* a new answer, for the PSP's volume bar */
} PspExpLocalAudio;

void pspexp_la_init(PspExpLocalAudio *la);
void pspexp_la_close(PspExpLocalAudio *la);
/* 1 while sound should go to the board's output and can (pw-play running) */
int  pspexp_la_active(PspExpLocalAudio *la, uint32_t rate, uint64_t now_us);
int  pspexp_la_write(PspExpLocalAudio *la, const int16_t *stereo, uint32_t frames);
void pspexp_la_reset(PspExpLocalAudio *la);
/* each input sample: the volume buttons (PSPEXP_BUTTON_VOLUP / VOLDOWN) set the
 * Bluetooth device's volume while the sound goes there; held, they repeat */
void pspexp_la_buttons(PspExpLocalAudio *la, uint32_t buttons, uint64_t now_us);

#endif
