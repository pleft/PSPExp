/*
 * Where the sound goes: the PSP, or a Bluetooth device paired with the board
 * (pspexp-bt). The console keeps the route file libpspexp reads in every
 * program (/tmp/pspexp-audio-route-UID), checking the chosen device every
 * few seconds, so the sound moves to the earbuds when they connect and back
 * to the PSP when they leave.
 */
#ifndef PSPEXP_TERM_BTAUDIO_H
#define PSPEXP_TERM_BTAUDIO_H

#include <stddef.h>
#include <stdint.h>

/* this board can (bluetoothctl, PipeWire's pw-play, a controller); a no is asked again every 20 s */
int  bt_available(void);
/* the main loop, each turn: the route kept current */
void bt_route_tick(uint64_t now);
/* check at the next turn (a device was just chosen) */
void bt_route_soon(void);
/* for System: "PSP", the device's name, or "PSP (NAME not connected)" */
void bt_route_label(char *out, size_t n);
/* for the top bar: 1 while the sound goes to a Bluetooth device, its volume
 * in *percent (-1 not known yet) */
int  bt_indicator(int *percent);
/* the chosen device's address, or "" for the PSP */
void bt_chosen(char *out, size_t n);

#endif
