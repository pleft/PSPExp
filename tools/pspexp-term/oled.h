/* A status panel on an SSD1306 128x64 OLED over I2C (oled.c). */
#ifndef PSPEXP_OLED_H
#define PSPEXP_OLED_H

#include <stdint.h>

#define OLED_LINES 8
#define OLED_COLS 21

/* 0 when the display answered (dev: an I2C bus, NULL for /dev/i2c-0) */
int  oled_open(const char *dev);
int  oled_active(void);
/* the text of one line (cut or padded to 21 characters) */
void oled_line(int row, const char *text);
/* draw the lines when they changed since the last flush */
void oled_flush(void);
/* blank the display */
void oled_close(void);

#endif
