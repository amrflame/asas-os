#include "console.h"

static const UINT8 letter_font[26][5] = {
    { 0x7E, 0x09, 0x09, 0x09, 0x7E }, { 0x7F, 0x49, 0x49, 0x49, 0x36 },
    { 0x3E, 0x41, 0x41, 0x41, 0x22 }, { 0x7F, 0x41, 0x41, 0x22, 0x1C },
    { 0x7F, 0x49, 0x49, 0x49, 0x41 }, { 0x7F, 0x09, 0x09, 0x09, 0x01 },
    { 0x3E, 0x41, 0x49, 0x49, 0x7A }, { 0x7F, 0x08, 0x08, 0x08, 0x7F },
    { 0x00, 0x41, 0x7F, 0x41, 0x00 }, { 0x20, 0x40, 0x41, 0x3F, 0x01 },
    { 0x7F, 0x08, 0x14, 0x22, 0x41 }, { 0x7F, 0x40, 0x40, 0x40, 0x40 },
    { 0x7F, 0x02, 0x0C, 0x02, 0x7F }, { 0x7F, 0x04, 0x08, 0x10, 0x7F },
    { 0x3E, 0x41, 0x41, 0x41, 0x3E }, { 0x7F, 0x09, 0x09, 0x09, 0x06 },
    { 0x3E, 0x41, 0x51, 0x21, 0x5E }, { 0x7F, 0x09, 0x19, 0x29, 0x46 },
    { 0x46, 0x49, 0x49, 0x49, 0x31 }, { 0x01, 0x01, 0x7F, 0x01, 0x01 },
    { 0x3F, 0x40, 0x40, 0x40, 0x3F }, { 0x1F, 0x20, 0x40, 0x20, 0x1F },
    { 0x3F, 0x40, 0x38, 0x40, 0x3F }, { 0x63, 0x14, 0x08, 0x14, 0x63 },
    { 0x07, 0x08, 0x70, 0x08, 0x07 }, { 0x61, 0x51, 0x49, 0x45, 0x43 }
};

static const UINT8 digit_font[10][5] = {
    { 0x3E, 0x51, 0x49, 0x45, 0x3E }, { 0x00, 0x42, 0x7F, 0x40, 0x00 },
    { 0x42, 0x61, 0x51, 0x49, 0x46 }, { 0x21, 0x41, 0x45, 0x4B, 0x31 },
    { 0x18, 0x14, 0x12, 0x7F, 0x10 }, { 0x27, 0x45, 0x45, 0x45, 0x39 },
    { 0x3C, 0x4A, 0x49, 0x49, 0x30 }, { 0x01, 0x71, 0x09, 0x05, 0x03 },
    { 0x36, 0x49, 0x49, 0x49, 0x36 }, { 0x06, 0x49, 0x49, 0x29, 0x1E }
};

static UINT8 glyph_column(char character, UINT32 column)
{
    if (character >= 'a' && character <= 'z') {
        character = (char)(character - 'a' + 'A');
    }

    if (character >= 'A' && character <= 'Z') {
        return letter_font[character - 'A'][column];
    }

    if (character >= '0' && character <= '9') {
        return digit_font[character - '0'][column];
    }

    if (character == '-') {
        return column == 2 ? 0x08 : 0x08;
    }

    if (character == ':') {
        return column == 2 ? 0x24 : 0;
    }

    if (character == '.') {
        return column == 2 ? 0x40 : 0;
    }

    return 0;
}

static void draw_character(ASAS_CONSOLE *console, char character)
{
    UINT32 column;
    UINT32 row;
    UINT32 scale_x;
    UINT32 scale_y;

    for (column = 0; column < 5; column++) {
        UINT8 bits = glyph_column(character, column);

        for (row = 0; row < 7; row++) {
            UINT32 color = (bits & (1U << row)) != 0 ? console->foreground : console->background;

            for (scale_y = 0; scale_y < console->scale; scale_y++) {
                for (scale_x = 0; scale_x < console->scale; scale_x++) {
                    framebuffer_put_pixel(
                        console->framebuffer,
                        console->cursor_x + column * console->scale + scale_x,
                        console->cursor_y + row * console->scale + scale_y,
                        color
                    );
                }
            }
        }
    }
}

void console_initialize(ASAS_CONSOLE *console, ASAS_FRAMEBUFFER *framebuffer)
{
    console->framebuffer = framebuffer;
    console->cursor_x = 24;
    console->cursor_y = 24;
    console->foreground = 0xE6EDF3;
    console->background = 0x07111F;
    console->scale = 2;
}

void console_clear(ASAS_CONSOLE *console)
{
    framebuffer_clear(console->framebuffer, console->background);
    console->cursor_x = 24;
    console->cursor_y = 24;
}

void console_write(ASAS_CONSOLE *console, const char *text)
{
    UINT32 character_width = 6 * console->scale;
    UINT32 line_height = 9 * console->scale;

    while (*text != '\0') {
        if (*text == '\n') {
            console->cursor_x = 24;
            console->cursor_y += line_height;
        } else {
            if (console->cursor_x + character_width >= console->framebuffer->width) {
                console->cursor_x = 24;
                console->cursor_y += line_height;
            }

            draw_character(console, *text);
            console->cursor_x += character_width;
        }

        text++;
    }
}
