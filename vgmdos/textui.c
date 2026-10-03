#include <conio.h>
#include <string.h>
#include "textui.h"

static unsigned char far *screen = 0;

static void ensure_screen(void)
{
    if (!screen)
        screen = (unsigned char far *)MK_FP(0xB800, 0);
}

void tui_clear(unsigned char attr)
{
    int i;
    ensure_screen();
    for (i = 0; i < TUI_COLS * TUI_ROWS; i++) {
        screen[i * 2] = ' ';
        screen[i * 2 + 1] = attr;
    }
}

void tui_fill(unsigned char ch, unsigned char attr)
{
    int i;
    ensure_screen();
    for (i = 0; i < TUI_COLS * TUI_ROWS; i++) {
        screen[i * 2] = (char)ch;
        screen[i * 2 + 1] = attr;
    }
}

void tui_putc(int row, int col, char ch, unsigned char attr)
{
    long off;
    ensure_screen();
    if (row < 0 || row >= TUI_ROWS || col < 0 || col >= TUI_COLS) return;
    off = ((long)row * TUI_COLS + col) * 2;
    screen[off] = ch;
    screen[off + 1] = attr;
}

void tui_setattr(int row, int col, unsigned char attr)
{
    long off;
    ensure_screen();
    if (row < 0 || row >= TUI_ROWS || col < 0 || col >= TUI_COLS) return;
    off = ((long)row * TUI_COLS + col) * 2;
    screen[off + 1] = attr;
}

void tui_puts(int row, int col, const char *s, unsigned char attr)
{
    int c = col;
    while (*s && c < TUI_COLS) {
        tui_putc(row, c, *s, attr);
        s++; c++;
    }
}

void tui_puts_padded(int row, int col, const char *s, int width, unsigned char attr)
{
    int i, len = (int)strlen(s);
    for (i = 0; i < width; i++) {
        char ch = (i < len) ? s[i] : ' ';
        tui_putc(row, col + i, ch, attr);
    }
}

void tui_box(int row, int col, int w, int h, unsigned char attr)
{
    int i;
    tui_putc(row, col, (char)0xC9, attr);
    tui_putc(row, col + w - 1, (char)0xBB, attr);
    tui_putc(row + h - 1, col, (char)0xC8, attr);
    tui_putc(row + h - 1, col + w - 1, (char)0xBC, attr);
    for (i = 1; i < w - 1; i++) {
        tui_putc(row, col + i, (char)0xCD, attr);
        tui_putc(row + h - 1, col + i, (char)0xCD, attr);
    }
    for (i = 1; i < h - 1; i++) {
        tui_putc(row + i, col, (char)0xBA, attr);
        tui_putc(row + i, col + w - 1, (char)0xBA, attr);
    }
}

void tui_hbar(int row, int col, int width, int filled,
              unsigned char attr_filled, unsigned char attr_empty)
{
    int i;
    if (filled > width) filled = width;
    if (filled < 0) filled = 0;
    for (i = 0; i < width; i++)
        tui_putc(row, col + i, (char)0xDB  ,
                 i < filled ? attr_filled : attr_empty);
}

int tui_getkey(void)
{
    int c = getch();
    if (c == 0 || c == 0xE0) {
        int c2 = getch();
        return -c2;
    }
    return c;
}
