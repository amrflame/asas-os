#ifndef ASAS_FRAMEBUFFER_H
#define ASAS_FRAMEBUFFER_H

#include "uefi.h"
#include "boot_info.h"

typedef struct {
    UINT32 *base;
    UINTN size;
    UINT32 width;
    UINT32 height;
    UINT32 stride;
    UINT32 pixel_format;
} ASAS_FRAMEBUFFER;

void framebuffer_initialize(
    ASAS_FRAMEBUFFER *framebuffer,
    const ASAS_BOOT_INFO *boot_info
);

void framebuffer_clear(ASAS_FRAMEBUFFER *framebuffer, UINT32 color);

void framebuffer_put_pixel(
    ASAS_FRAMEBUFFER *framebuffer,
    UINT32 x,
    UINT32 y,
    UINT32 color
);

void framebuffer_fill_rect(
    ASAS_FRAMEBUFFER *framebuffer,
    UINT32 x,
    UINT32 y,
    UINT32 width,
    UINT32 height,
    UINT32 color
);

#endif
