#ifndef ASAS_GUI_FONT_H
#define ASAS_GUI_FONT_H

/*
 * gui_font.h — PSF2 bitmap font loader for ASAS Desktop Environment
 *
 * Loads a PC Screen Font v2 (.psf) file from the FAT16 disk via VFS.
 * Falls back to the built-in 5×7 font (gui_draw.c) if loading fails.
 *
 * PSF2 layout:
 *   4-byte magic: 0x72 0xb5 0x4a 0x86
 *   UINT32 version, header_size, flags, glyph_count, bytes_per_glyph,
 *          height, width
 *   Then glyph_count × bytes_per_glyph bytes of bitmap data.
 *   Each glyph row is ceil(width/8) bytes, MSB first.
 *
 * Usage:
 *   gui_font_load("/fonts/terminus16.psf");   // call once at init
 *   gui_font_draw_char(x, y, 'A', color);    // replaces gui_draw_char
 *   gui_font_draw_text(x, y, "hello", color);
 */

#include "uefi.h"

/* Maximum glyph count we support (covers full Latin + basic symbols). */
#define GUI_FONT_MAX_GLYPHS 512

/* Maximum glyph pixel size. Terminus 16/32 fits inside 32×32. */
#define GUI_FONT_MAX_W 32
#define GUI_FONT_MAX_H 32

/* Maximum total bitmap bytes (512 glyphs × 32×32/8 = 65536 bytes). */
#define GUI_FONT_BITMAP_BYTES (GUI_FONT_MAX_GLYPHS * GUI_FONT_MAX_H * 4)

/*
 * Attempt to load a PSF2 font from disk path.
 * Returns 1 on success (font is active), 0 on failure (built-in stays active).
 * Safe to call multiple times — replaces any previously loaded font.
 */
int gui_font_load(const char *path);

/* Returns 1 if a PSF2 font is currently loaded and active. */
int gui_font_ready(void);

/* Returns loaded font glyph width/height (or 5/7 if using built-in). */
UINT32 gui_font_glyph_w(void);
UINT32 gui_font_glyph_h(void);

/*
 * Draw a single character at (x,y) with the loaded font (or built-in fallback).
 * Drop-in replacement for gui_draw_char().
 */
void gui_font_draw_char(int x, int y, char ch, UINT32 color);

/*
 * Draw null-terminated string. Advance x by (glyph_w+1) per character.
 * Drop-in replacement for gui_draw_text().
 */
void gui_font_draw_text(int x, int y, const char *text, UINT32 color);

/* Return pixel width of text string with loaded font. */
UINT32 gui_font_text_width(const char *text);

#endif /* ASAS_GUI_FONT_H */
