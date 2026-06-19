/*
 * gui_font.c — PSF2 bitmap font loader for ASAS Desktop Environment
 *
 * Reads a PSF2 font file from the VFS, stores the bitmap in a static buffer,
 * and draws glyphs pixel-by-pixel into the compositor backbuffer via gui_put_pixel.
 * Falls back to gui_draw_char() if no PSF2 font is loaded.
 */

#include "gui_font.h"
#include "gui_draw.h"
#include "gui_theme.h"
#include "vfs.h"
#include "logger.h"

/* ======================================================================
 * PSF2 header (packed on-disk layout)
 * ====================================================================== */
#pragma pack(push, 1)
typedef struct {
    UINT8  magic[4];        /* 0x72 0xB5 0x4A 0x86 */
    UINT32 version;         /* must be 0 */
    UINT32 header_size;     /* offset to glyph data */
    UINT32 flags;           /* 0x01 = has unicode table */
    UINT32 glyph_count;
    UINT32 bytes_per_glyph;
    UINT32 height;          /* rows per glyph */
    UINT32 width;           /* columns per glyph */
} PSF2_HEADER;
#pragma pack(pop)

static const UINT8 PSF2_MAGIC[4] = { 0x72, 0xB5, 0x4A, 0x86 };

/* ======================================================================
 * Static font storage — large enough for Terminus 16 (256 glyphs × 32B)
 * ====================================================================== */
static UINT8  s_bitmap[GUI_FONT_BITMAP_BYTES];
static UINT32 s_glyph_w;
static UINT32 s_glyph_h;
static UINT32 s_bytes_per_glyph;
static UINT32 s_glyph_count;
static int    s_ready;

/* Bytes per row = ceil(width / 8) */
static UINT32 s_bytes_per_row;

/* ======================================================================
 * Internal: memcmp, memcpy without stdlib
 * ====================================================================== */
static int psf_memcmp(const void *a, const void *b, UINT32 n)
{
    const UINT8 *pa = (const UINT8 *)a;
    const UINT8 *pb = (const UINT8 *)b;
    UINT32 i;
    for (i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

/* ======================================================================
 * Public API
 * ====================================================================== */

int gui_font_load(const char *path)
{
    static UINT8 file_buf[4 + sizeof(PSF2_HEADER) + GUI_FONT_BITMAP_BYTES];
    PSF2_HEADER  hdr;
    UINT64 handle, n;
    UINT32 bitmap_bytes;
    UINT32 i;

    s_ready = 0;

    handle = vfs_open(path);
    if (!handle) {
        logger_write("WARN", "gui_font: font file not found");
        return 0;
    }

    /* Read the entire file into file_buf */
    n = vfs_read(handle, file_buf, sizeof(file_buf) - 1);
    vfs_close(handle);

    if (n < sizeof(PSF2_HEADER)) {
        logger_write("WARN", "gui_font: file too small");
        return 0;
    }

    /* Copy header out (avoids alignment issues with pragma pack) */
    for (i = 0; i < sizeof(PSF2_HEADER); i++)
        ((UINT8 *)&hdr)[i] = file_buf[i];

    /* Validate magic */
    if (psf_memcmp(hdr.magic, PSF2_MAGIC, 4) != 0) {
        logger_write("WARN", "gui_font: bad PSF2 magic");
        return 0;
    }
    if (hdr.version != 0) {
        logger_write("WARN", "gui_font: unsupported PSF2 version");
        return 0;
    }
    if (hdr.width == 0 || hdr.height == 0 || hdr.glyph_count == 0) {
        logger_write("WARN", "gui_font: invalid glyph dimensions");
        return 0;
    }
    if (hdr.width > GUI_FONT_MAX_W || hdr.height > GUI_FONT_MAX_H) {
        logger_write("WARN", "gui_font: glyph too large");
        return 0;
    }

    UINT32 safe_count = hdr.glyph_count;
    if (safe_count > GUI_FONT_MAX_GLYPHS) safe_count = GUI_FONT_MAX_GLYPHS;

    bitmap_bytes = safe_count * hdr.bytes_per_glyph;
    if (bitmap_bytes > GUI_FONT_BITMAP_BYTES) {
        logger_write("WARN", "gui_font: bitmap too large for buffer");
        return 0;
    }

    if (n < (UINT64)hdr.header_size + bitmap_bytes) {
        logger_write("WARN", "gui_font: file truncated");
        return 0;
    }

    /* Copy bitmap data */
    {
        const UINT8 *src = file_buf + hdr.header_size;
        for (i = 0; i < bitmap_bytes; i++) s_bitmap[i] = src[i];
    }

    s_glyph_w        = hdr.width;
    s_glyph_h        = hdr.height;
    s_bytes_per_glyph = hdr.bytes_per_glyph;
    s_glyph_count    = safe_count;
    s_bytes_per_row  = (hdr.width + 7u) / 8u;
    s_ready          = 1;

    logger_write("INFO", "gui_font: PSF2 font loaded");
    return 1;
}

int gui_font_ready(void)
{
    return s_ready;
}

UINT32 gui_font_glyph_w(void)
{
    return s_ready ? s_glyph_w : (UINT32)FONT_W;
}

UINT32 gui_font_glyph_h(void)
{
    return s_ready ? s_glyph_h : (UINT32)FONT_H;
}

void gui_font_draw_char(int x, int y, char ch, UINT32 color)
{
    UINT32 glyph_idx, row, col;
    const UINT8 *glyph;

    if (!s_ready) {
        gui_draw_char(x, y, ch, color);
        return;
    }

    /* Map char to glyph index — simple ASCII mapping */
    glyph_idx = (UINT32)(unsigned char)ch;
    if (glyph_idx >= s_glyph_count) glyph_idx = 0; /* glyph 0 = .notdef */

    glyph = s_bitmap + glyph_idx * s_bytes_per_glyph;

    for (row = 0; row < s_glyph_h; row++) {
        const UINT8 *row_data = glyph + row * s_bytes_per_row;
        for (col = 0; col < s_glyph_w; col++) {
            /* MSB first: byte col/8, bit (7 - col%8) */
            UINT8 byte = row_data[col / 8u];
            UINT8 mask = (UINT8)(0x80u >> (col % 8u));
            if (byte & mask) {
                gui_put_pixel(x + (int)col, y + (int)row, color);
            }
        }
    }
}

void gui_font_draw_text(int x, int y, const char *text, UINT32 color)
{
    UINT32 advance = (s_ready ? s_glyph_w : (UINT32)FONT_W) + 1u;
    while (*text) {
        gui_font_draw_char(x, y, *text++, color);
        x += (int)advance;
    }
}

UINT32 gui_font_text_width(const char *text)
{
    UINT32 n = 0;
    UINT32 advance = (s_ready ? s_glyph_w : (UINT32)FONT_W) + 1u;
    while (*text++) n++;
    return n * advance;
}
