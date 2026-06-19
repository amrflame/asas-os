#ifndef ASAS_GFX_H
#define ASAS_GFX_H

/*
 * gfx.h — Graphics acceleration abstraction layer for ASAS OS
 *
 * Provides a unified API over GPU (VirtIO) or CPU (memcpy) rendering paths.
 * Call gfx_probe_gpu() once after PCI discovery, then gfx_attach_backbuf()
 * when the compositor allocates its backbuffer. After that, gfx_flush()
 * will use DMA if a GPU is available, otherwise it is a no-op (the compositor
 * falls back to its own CPU blit in gui_compositor_blit).
 *
 * Usage in main.c:
 *   pci_discover_devices(&pci_registry);
 *   gfx_probe_gpu(&pci_registry);       // detects VirtIO GPU
 *   ...
 *   gui_initialize(fb);                  // internally calls gfx_attach_backbuf
 *
 * Usage in gui_compositor.c:
 *   gui_compositor_blit()  →  gfx_flush() if gfx_gpu_active(), else CPU copy
 */

#include "uefi.h"
#include "pci.h"

/*
 * Scan PCI registry for a VirtIO GPU (vendor=0x1AF4, device=0x1050).
 * Stores the device pointer internally. Must be called before gfx_attach_backbuf.
 */
void gfx_probe_gpu(const ASAS_PCI_REGISTRY *registry);

/*
 * Attach compositor backbuffer to the GFX layer.
 * If a VirtIO GPU was found, initializes the GPU resource and scanout.
 * Called from gui_compositor_initialize().
 */
void gfx_attach_backbuf(UINT32 *buf, UINT32 width, UINT32 height, UINT32 stride);

/*
 * Push dirty rectangle from backbuffer to display via GPU DMA.
 * No-op if GPU is not active (compositor CPU path handles it separately).
 */
void gfx_flush(UINT32 x, UINT32 y, UINT32 w, UINT32 h);

/* Returns 1 if GPU path is active (VirtIO GPU initialized successfully). */
int gfx_gpu_active(void);

#endif /* ASAS_GFX_H */
