#ifndef TEXTUI_H
#define TEXTUI_H

#include <dos.h>

#define TUI_COLS 80
#define TUI_ROWS 25

#define TUI_WHITE_ON_BLUE   0x1F
#define TUI_YELLOW_ON_BLUE  0x1E
#define TUI_CYAN_ON_BLUE    0x1B
#define TUI_GREEN_ON_BLUE   0x1A
#define TUI_WHITE_ON_BLACK  0x0F
#define TUI_BLACK_ON_WHITE  0x70
#define TUI_RED_ON_BLACK    0x0C
#define TUI_GREEN_ON_BLACK  0x0A

#define TUI_CYAN_ON_BLACK    0x0B
#define TUI_BLUE_ON_BLACK    0x09
#define TUI_DARKBLUE_ON_BLACK 0x01
#define TUI_YELLOW_ON_BLACK  0x0E
#define TUI_BLACK_ON_CYAN    0xB0
#define TUI_GRAY_ON_BLACK    0x08
#define TUI_MAGENTA_ON_BLACK 0x0D
#define TUI_WHITE_ON_RED      0x4F
#define TUI_BROWN_ON_BLACK    0x04

#define TUI_BLINK(attr) ((unsigned char)((attr) | 0x80))

#define TUI_KEY_UP     (-72)
#define TUI_KEY_DOWN   (-80)
#define TUI_KEY_LEFT   (-75)
#define TUI_KEY_RIGHT  (-77)
#define TUI_KEY_HOME   (-71)
#define TUI_KEY_END    (-79)
#define TUI_KEY_PGUP   (-73)
#define TUI_KEY_PGDN   (-81)
#define TUI_KEY_ESC    27
#define TUI_KEY_ENTER  13

void tui_clear(unsigned char attr);
void tui_putc(int row, int col, char ch, unsigned char attr);

void tui_setattr(int row, int col, unsigned char attr);
void tui_puts(int row, int col, const char *s, unsigned char attr);

void tui_puts_padded(int row, int col, const char *s, int width, unsigned char attr);
void tui_box(int row, int col, int w, int h, unsigned char attr);

void tui_hbar(int row, int col, int width, int filled,
              unsigned char attr_filled, unsigned char attr_empty);

int tui_getkey(void);

#endif
