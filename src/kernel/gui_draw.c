/*
 * gui_draw.c — 2-D drawing primitives for ASAS Desktop Environment
 *
 * Uses a target buffer set once via gui_draw_set_target().
 * Font: 5x7 column-major bitmap.
 */

#include "gui_draw.h"
#include "gui_theme.h"

/* ======================================================================
 * Back-buffer target
 * ====================================================================== */
static UINT32 *g_buf;
static UINT32  g_width;
static UINT32  g_height;
static UINT32  g_stride;

void gui_draw_set_target(UINT32 *buf, UINT32 width, UINT32 height, UINT32 stride)
{
    g_buf    = buf;
    g_width  = width;
    g_height = height;
    g_stride = stride;
}

/* ======================================================================
 * 5x7 column-major bitmap font
 * Each entry = 5 bytes (one per column), bit0=top row, bit6=bottom row.
 * ====================================================================== */
static const UINT8 s_font_upper[26][5] = {
    {0x3E,0x09,0x09,0x09,0x3E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x01,0x01}, /* F */
    {0x3E,0x41,0x41,0x51,0x72}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x03,0x04,0x78,0x04,0x03}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
};

static const UINT8 s_font_digit[10][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
};

static UINT8 glyph_col(char ch, UINT32 col)
{
    if (col >= 5) return 0;
    if (ch >= 'A' && ch <= 'Z') return s_font_upper[(UINT8)(ch - 'A')][col];
    if (ch >= 'a' && ch <= 'z') return s_font_upper[(UINT8)(ch - 'a')][col];
    if (ch >= '0' && ch <= '9') return s_font_digit[(UINT8)(ch - '0')][col];
    /* Common punctuation */
    switch (ch) {
    case ' ':  return 0x00;
    case '.':  return (col == 1 || col == 2) ? 0x40 : 0x00;
    case ',':  return (col == 1) ? 0x60 : (col == 2) ? 0x20 : 0x00;
    case ':':  return (col == 2) ? 0x24 : 0x00;
    case ';':  return (col == 2) ? 0x56 : 0x00;
    case '!':  return (col == 2) ? 0x5F : 0x00;
    case '?':  return col==0?0x02:col==1?0x01:col==2?0x51:col==3?0x09:0x06;
    case '-':  return (col >= 1 && col <= 3) ? 0x08 : 0x00;
    case '_':  return 0x40;
    case '+':  return col==0?0x08:col==1?0x08:col==2?0x3E:col==3?0x08:0x08;
    case '*':  return col==0?0x14:col==1?0x08:col==2?0x3E:col==3?0x08:0x14;
    case '=':  return 0x14;
    case '/':  return col==0?0x20:col==1?0x10:col==2?0x08:col==3?0x04:0x02;
    case '\\': return col==0?0x02:col==1?0x04:col==2?0x08:col==3?0x10:0x20;
    case '|':  return (col == 2) ? 0x7F : 0x00;
    case '<':  return col==0?0x00:col==1?0x08:col==2?0x14:col==3?0x22:0x41;
    case '>':  return col==0?0x41:col==1?0x22:col==2?0x14:col==3?0x08:0x00;
    case '(':  return col==0?0x00:col==1?0x1C:col==2?0x22:col==3?0x41:0x00;
    case ')':  return col==0?0x00:col==1?0x41:col==2?0x22:col==3?0x1C:0x00;
    case '[':  return col==0?0x00:col==1?0x7F:col==2?0x41:col==3?0x41:0x00;
    case ']':  return col==0?0x00:col==1?0x41:col==2?0x41:col==3?0x7F:0x00;
    case '{':  return col==0?0x08:col==1?0x36:col==2?0x41:col==3?0x00:0x00;
    case '}':  return col==0?0x00:col==1?0x00:col==2?0x41:col==3?0x36:0x08;
    case '\'': return (col == 2) ? 0x03 : 0x00;
    case '"':  return (col == 1 || col == 3) ? 0x03 : 0x00;
    case '#':  return col==0?0x14:col==1?0x7F:col==2?0x14:col==3?0x7F:0x14;
    case '$':  return col==0?0x24:col==1?0x2A:col==2?0x7F:col==3?0x2A:0x12;
    case '%':  return col==0?0x63:col==1?0x13:col==2?0x08:col==3?0x64:0x63;
    case '@':  return col==0?0x3E:col==1?0x41:col==2?0x5D:col==3?0x55:0x1E;
    case '^':  return col==0?0x02:col==1?0x01:col==2?0x01:col==3?0x02:0x00;
    case '~':  return col==0?0x02:col==1?0x01:col==2?0x02:col==3?0x04:0x02;
    case '`':  return (col == 1) ? 0x01 : (col == 2) ? 0x02 : 0x00;
    default:   return 0x00;
    }
}

/* ======================================================================
 * Primitives
 * ====================================================================== */
void gui_put_pixel(int x, int y, UINT32 color)
{
    if (!g_buf || x < 0 || y < 0) return;
    if ((UINT32)x >= g_width || (UINT32)y >= g_height) return;
    g_buf[(UINT32)y * g_stride + (UINT32)x] = color;
}

void gui_fill_rect(int x, int y, UINT32 w, UINT32 h, UINT32 color)
{
    UINT32 row, col;

    if (!g_buf) return;
    if (x < 0) { if ((UINT32)(-x) >= w) return; w -= (UINT32)(-x); x = 0; }
    if (y < 0) { if ((UINT32)(-y) >= h) return; h -= (UINT32)(-y); y = 0; }
    if ((UINT32)x >= g_width || (UINT32)y >= g_height) return;
    if ((UINT32)x + w > g_width)  w = g_width  - (UINT32)x;
    if ((UINT32)y + h > g_height) h = g_height - (UINT32)y;
    if (w == 0 || h == 0) return;

    for (row = 0; row < h; row++) {
        UINT32 *line = g_buf + ((UINT32)y + row) * g_stride + (UINT32)x;
        for (col = 0; col < w; col++) line[col] = color;
    }
}

void gui_draw_border(int x, int y, UINT32 w, UINT32 h, UINT32 color)
{
    gui_fill_rect(x,            y,            w, 1, color);
    gui_fill_rect(x,            y+(int)h-1,   w, 1, color);
    gui_fill_rect(x,            y,            1, h, color);
    gui_fill_rect(x+(int)w-1,   y,            1, h, color);
}

void gui_fill_gradient_v(int x, int y, UINT32 w, UINT32 h,
                          UINT32 color_top, UINT32 color_bot)
{
    UINT32 r;
    UINT32 r0 = (color_top >> 16) & 0xFF, r1 = (color_bot >> 16) & 0xFF;
    UINT32 g0 = (color_top >>  8) & 0xFF, g1 = (color_bot >>  8) & 0xFF;
    UINT32 b0 = (color_top      ) & 0xFF, b1 = (color_bot      ) & 0xFF;

    for (r = 0; r < h; r++) {
        UINT32 t   = r * 255 / (h > 1 ? h - 1 : 1);
        UINT32 rc  = r0 + (r1 - r0) * t / 255;
        UINT32 gc  = g0 + (g1 - g0) * t / 255;
        UINT32 bc  = b0 + (b1 - b0) * t / 255;
        UINT32 col = 0xFF000000 | (rc << 16) | (gc << 8) | bc;
        gui_fill_rect(x, y + (int)r, w, 1, col);
    }
}

void gui_fill_rounded(int x, int y, UINT32 w, UINT32 h,
                       UINT32 color, UINT32 corner_r)
{
    UINT32 r = corner_r;
    if (r == 0 || w < 2*r || h < 2*r) { gui_fill_rect(x, y, w, h, color); return; }
    /* Main body */
    gui_fill_rect(x + (int)r, y,           w - 2*r, h,       color);
    gui_fill_rect(x,           y + (int)r, (int)r,  h - 2*r, color);
    gui_fill_rect(x + (int)w - (int)r, y + (int)r, (int)r, h - 2*r, color);
    /* Corners (pixel approximation) */
    if (r >= 1) {
        gui_put_pixel(x + (int)r - 1, y + 1, color);
        gui_put_pixel(x + (int)w - (int)r, y + 1, color);
        gui_put_pixel(x + (int)r - 1, y + (int)h - 2, color);
        gui_put_pixel(x + (int)w - (int)r, y + (int)h - 2, color);
    }
    if (r >= 2) {
        gui_put_pixel(x + 1, y + (int)r - 1, color);
        gui_put_pixel(x + (int)w - 2, y + (int)r - 1, color);
        gui_put_pixel(x + 1, y + (int)h - (int)r, color);
        gui_put_pixel(x + (int)w - 2, y + (int)h - (int)r, color);
    }
}

void gui_draw_border_rounded(int x, int y, UINT32 w, UINT32 h,
                              UINT32 color, UINT32 corner_r)
{
    UINT32 r = corner_r;
    if (r == 0 || w < 2*r || h < 2*r) { gui_draw_border(x, y, w, h, color); return; }
    gui_fill_rect(x + (int)r,       y,              w - 2*r, 1, color);
    gui_fill_rect(x + (int)r,       y + (int)h - 1, w - 2*r, 1, color);
    gui_fill_rect(x,                y + (int)r,     1, h - 2*r, color);
    gui_fill_rect(x + (int)w - 1,   y + (int)r,     1, h - 2*r, color);
    if (r >= 1) {
        gui_put_pixel(x + (int)r - 1, y + 1, color);
        gui_put_pixel(x + (int)w - (int)r, y + 1, color);
        gui_put_pixel(x + (int)r - 1, y + (int)h - 2, color);
        gui_put_pixel(x + (int)w - (int)r, y + (int)h - 2, color);
    }
}

/* ======================================================================
 * Text drawing
 * ====================================================================== */
void gui_draw_char(int x, int y, char ch, UINT32 color)
{
    UINT32 col, row;
    for (col = 0; col < FONT_W; col++) {
        UINT8 bits = glyph_col(ch, col);
        for (row = 0; row < FONT_H; row++) {
            if (bits & (1U << row)) gui_put_pixel(x+(int)col, y+(int)row, color);
        }
    }
}

void gui_draw_text(int x, int y, const char *text, UINT32 color)
{
    while (*text) { gui_draw_char(x, y, *text++, color); x += CELL_W; }
}

void gui_draw_text_n(int x, int y, const char *text, UINT32 max_len, UINT32 color)
{
    UINT32 i = 0;
    while (*text && i < max_len) { gui_draw_char(x, y, *text++, color); x += CELL_W; i++; }
}

UINT32 gui_text_width(const char *text)
{
    UINT32 n = 0;
    while (*text++) n++;
    return n * CELL_W;
}

void gui_draw_text_centered(int rx, int ry, UINT32 rw, UINT32 rh,
                             const char *text, UINT32 color)
{
    UINT32 tw = gui_text_width(text);
    int tx = rx + ((int)rw - (int)tw) / 2;
    int ty = ry + ((int)rh - FONT_H) / 2;
    gui_draw_text(tx, ty, text, color);
}

void gui_draw_text_right(int rx, int ry, UINT32 rw, const char *text, UINT32 color)
{
    UINT32 tw = gui_text_width(text);
    gui_draw_text(rx + (int)rw - (int)tw, ry, text, color);
}

/* ======================================================================
 * Integer helpers
 * ====================================================================== */
void gui_uint_to_str(UINT32 v, char *buf, UINT32 buf_sz)
{
    char tmp[12];
    UINT32 i = 0, j = 0;
    if (buf_sz == 0) return;
    if (v == 0) { if (buf_sz > 1) { buf[0]='0'; buf[1]='\0'; } return; }
    while (v && i < 11) { tmp[i++] = '0' + (char)(v % 10); v /= 10; }
    while (i > 0 && j + 1 < buf_sz) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

void gui_uint_to_hex(UINT32 v, char *buf, UINT32 buf_sz)
{
    const char *hex = "0123456789ABCDEF";
    UINT32 i, j = 0;
    if (buf_sz < 3) return;
    for (i = 28; j + 1 < buf_sz; i -= 4) {
        buf[j++] = hex[(v >> i) & 0xF];
        if (i == 0) break;
    }
    buf[j] = '\0';
}

void gui_format_time(UINT32 ticks_100hz, char *buf, UINT32 buf_sz)
{
    /* ticks_100hz increments at 100 Hz: 100 ticks = 1 s */
    UINT32 secs = ticks_100hz / 100;
    UINT32 hh   = (secs / 3600) % 24;
    UINT32 mm   = (secs / 60) % 60;
    UINT32 ss   = secs % 60;
    if (buf_sz < 9) return;
    buf[0] = '0' + (char)(hh / 10); buf[1] = '0' + (char)(hh % 10); buf[2] = ':';
    buf[3] = '0' + (char)(mm / 10); buf[4] = '0' + (char)(mm % 10); buf[5] = ':';
    buf[6] = '0' + (char)(ss / 10); buf[7] = '0' + (char)(ss % 10); buf[8] = '\0';
}

