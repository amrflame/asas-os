#include "framebuffer.h"

static UINT32 native_color(const ASAS_FRAMEBUFFER *framebuffer, UINT32 color)
{
    UINT32 red = (color >> 16) & 0xFF;
    UINT32 green = (color >> 8) & 0xFF;
    UINT32 blue = color & 0xFF;

    if (framebuffer->pixel_format == 1) {
        return blue << 16 | green << 8 | red;
    }

    return red << 16 | green << 8 | blue;
}

void framebuffer_initialize(
    ASAS_FRAMEBUFFER *framebuffer,
    const ASAS_BOOT_INFO *boot_info
)
{
    framebuffer->base = (UINT32 *)(UINTN)boot_info->framebuffer_base;
    framebuffer->size = boot_info->framebuffer_size;
    framebuffer->width = boot_info->framebuffer_width;
    framebuffer->height = boot_info->framebuffer_height;
    framebuffer->stride = boot_info->framebuffer_stride;
    framebuffer->pixel_format = boot_info->framebuffer_pixel_format;
}

void framebuffer_clear(ASAS_FRAMEBUFFER *framebuffer, UINT32 color)
{
    framebuffer_fill_rect(
        framebuffer,
        0,
        0,
        framebuffer->width,
        framebuffer->height,
        color
    );
}

void framebuffer_put_pixel(
    ASAS_FRAMEBUFFER *framebuffer,
    UINT32 x,
    UINT32 y,
    UINT32 color
)
{
    if (x >= framebuffer->width || y >= framebuffer->height) {
        return;
    }

    framebuffer->base[y * framebuffer->stride + x] = native_color(framebuffer, color);
}

void framebuffer_fill_rect(
    ASAS_FRAMEBUFFER *framebuffer,
    UINT32 x,
    UINT32 y,
    UINT32 width,
    UINT32 height,
    UINT32 color
)
{
    UINT32 row;
    UINT32 column;
    UINT32 end_x = x + width;
    UINT32 end_y = y + height;
    UINT32 pixel = native_color(framebuffer, color);

    if (end_x > framebuffer->width) {
        end_x = framebuffer->width;
    }

    if (end_y > framebuffer->height) {
        end_y = framebuffer->height;
    }

    for (row = y; row < end_y; row++) {
        UINT32 *line = framebuffer->base + row * framebuffer->stride;

        for (column = x; column < end_x; column++) {
            line[column] = pixel;
        }
    }
}
