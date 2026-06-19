#ifndef ASAS_VIRTIO_GPU_H
#define ASAS_VIRTIO_GPU_H

/*
 * virtio_gpu.h — VirtIO GPU driver header for ASAS OS
 *
 * Supports VirtIO legacy PCI GPU (vendor=0x1AF4, device=0x1050).
 * Provides DMA-accelerated screen flush via TRANSFER_TO_HOST_2D + RESOURCE_FLUSH.
 * Used by QEMU (-device virtio-gpu-pci). Falls back gracefully if not found.
 */

#include "uefi.h"
#include "pci.h"

/*
 * Initialize VirtIO GPU.
 *   dev      — PCI device (vendor=0x1AF4, device=0x1050)
 *   backbuf  — CPU backbuffer pointer (compositor's back buffer)
 *   width    — framebuffer width in pixels
 *   height   — framebuffer height in pixels
 *   stride   — row stride in pixels (pixels per row, >= width)
 * Returns 1 on success (GPU active), 0 on failure.
 */
int virtio_gpu_initialize(const ASAS_PCI_DEVICE *dev,
                           UINT32 *backbuf,
                           UINT32 width, UINT32 height, UINT32 stride);

/*
 * Push a dirty rectangle from the backbuffer to the display via GPU DMA.
 * Call after drawing is complete, before presenting.
 *   x, y, w, h — dirty region (use 0,0,width,height for full screen)
 */
void virtio_gpu_flush(UINT32 x, UINT32 y, UINT32 w, UINT32 h);

/* Returns 1 if the VirtIO GPU driver was successfully initialized. */
int virtio_gpu_ready(void);

#endif /* ASAS_VIRTIO_GPU_H */
