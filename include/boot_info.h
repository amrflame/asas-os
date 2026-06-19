#ifndef ASAS_BOOT_INFO_H
#define ASAS_BOOT_INFO_H

#include "uefi.h"

#define ASAS_BOOT_INFO_MAGIC 0x53415341U
#define ASAS_BOOT_INFO_VERSION 1U

typedef struct {
    UINT32 magic;
    UINT32 version;
    UINT64 framebuffer_base;
    UINTN framebuffer_size;
    UINT32 framebuffer_width;
    UINT32 framebuffer_height;
    UINT32 framebuffer_stride;
    UINT32 framebuffer_pixel_format;
    UINT64 acpi_rsdp;
    UINT64 ap_trampoline;
    UINT64 boot_disk_text_base;
    UINT64 boot_disk_text_size;
    UINT64 boot_readme_base;
    UINT64 boot_readme_size;
    UINT64 boot_user_program_base;
    UINT64 boot_user_program_size;
} ASAS_BOOT_INFO;

#endif
