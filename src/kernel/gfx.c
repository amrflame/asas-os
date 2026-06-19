/*
 * gfx.c — Graphics acceleration abstraction for ASAS OS
 *
 * Sits between the compositor and the VirtIO GPU driver.
 * On QEMU with -device virtio-gpu-pci: GPU DMA path active.
 * On Hyper-V / real hardware without VirtIO GPU: CPU memcpy path used.
 */

#include "gfx.h"
#include "virtio_gpu.h"
#include "logger.h"

/* VirtIO GPU device ID */
#define PCI_DEVICE_VIRTIO_GPU 0x1050u

/* Stored device pointer from PCI scan */
static const ASAS_PCI_DEVICE *s_gpu_pci_dev;

/* ======================================================================
 * Public API
 * ====================================================================== */

void gfx_probe_gpu(const ASAS_PCI_REGISTRY *registry)
{
    s_gpu_pci_dev = pci_find_device(registry, PCI_VENDOR_VIRTIO, PCI_DEVICE_VIRTIO_GPU);
    if (s_gpu_pci_dev) {
        logger_write("INFO", "GFX: VirtIO GPU detected on PCI bus");
    } else {
        logger_write("INFO", "GFX: no VirtIO GPU; CPU framebuffer path in use");
    }
}

void gfx_attach_backbuf(UINT32 *buf, UINT32 width, UINT32 height, UINT32 stride)
{
    if (!s_gpu_pci_dev || !buf) return;

    if (virtio_gpu_initialize(s_gpu_pci_dev, buf, width, height, stride)) {
        logger_write("INFO", "GFX: GPU-accelerated display flush enabled");
    } else {
        logger_write("WARN", "GFX: VirtIO GPU init failed; falling back to CPU blit");
    }
}

void gfx_flush(UINT32 x, UINT32 y, UINT32 w, UINT32 h)
{
    virtio_gpu_flush(x, y, w, h);
}

int gfx_gpu_active(void)
{
    return virtio_gpu_ready();
}
