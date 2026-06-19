/*
 * virtio_gpu.c — VirtIO GPU driver for ASAS OS
 *
 * Implements the VirtIO legacy PCI GPU protocol (device 0x1AF4/0x1050).
 * Workflow:
 *   Init:  RESOURCE_CREATE_2D → RESOURCE_ATTACH_BACKING → SET_SCANOUT
 *   Flush: TRANSFER_TO_HOST_2D → RESOURCE_FLUSH
 *
 * All commands are synchronous (poll-wait). No interrupts needed.
 * Memory layout follows virtio_block.c conventions (same IO BAR, same queue).
 */

#include "virtio_gpu.h"
#include "logger.h"
#include "pci.h"

/* ---- Port I/O intrinsics (MSVC freestanding, same as virtio_block.c) ---- */
#pragma intrinsic(__inbyte)
unsigned char  __inbyte(unsigned short port);
#pragma intrinsic(__inword)
unsigned short __inword(unsigned short port);
#pragma intrinsic(__indword)
unsigned long  __indword(unsigned short port);
#pragma intrinsic(__outbyte)
void __outbyte(unsigned short port, unsigned char value);
#pragma intrinsic(__outword)
void __outword(unsigned short port, unsigned short value);
#pragma intrinsic(__outdword)
void __outdword(unsigned short port, unsigned long value);

/* ---- VirtIO legacy PCI register offsets ---- */
#define VIRTIO_PCI_HOST_FEATURES  0x00
#define VIRTIO_PCI_GUEST_FEATURES 0x04
#define VIRTIO_PCI_QUEUE_PFN      0x08
#define VIRTIO_PCI_QUEUE_NUM      0x0C
#define VIRTIO_PCI_QUEUE_SELECT   0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY   0x10
#define VIRTIO_PCI_STATUS         0x12

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4

/* ---- VirtQueue descriptor flags ---- */
#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2   /* device writes into this buffer */

/* ---- VirtIO GPU command types ---- */
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101u
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106u
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103u
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105u
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104u

/* BGRX8888: matches UEFI GOP framebuffer byte order on x86 LE */
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2u

/* ======================================================================
 * Command / response structures (packed for exact wire layout)
 * ====================================================================== */
#pragma pack(push, 1)

typedef struct {
    UINT32 type;
    UINT32 flags;
    UINT64 fence_id;
    UINT32 ctx_id;
    UINT32 padding;
} VGPU_HDR;

typedef struct {
    VGPU_HDR hdr;
    UINT32   resource_id;
    UINT32   format;
    UINT32   width;
    UINT32   height;
} VGPU_CREATE_2D;

typedef struct {
    UINT64 addr;      /* guest physical address */
    UINT32 length;
    UINT32 padding;
} VGPU_MEM_ENTRY;

typedef struct {
    VGPU_HDR       hdr;
    UINT32         resource_id;
    UINT32         nr_entries;
    VGPU_MEM_ENTRY entries[1]; /* exactly 1 entry (our single backbuffer) */
} VGPU_ATTACH_BACKING;

typedef struct { UINT32 x, y, width, height; } VGPU_RECT;

typedef struct {
    VGPU_HDR  hdr;
    VGPU_RECT r;
    UINT32    scanout_id;
    UINT32    resource_id;
} VGPU_SET_SCANOUT;

typedef struct {
    VGPU_HDR  hdr;
    VGPU_RECT r;
    UINT64    offset;       /* byte offset into resource = y*stride*4 */
    UINT32    resource_id;
    UINT32    padding;
} VGPU_TRANSFER_2D;

typedef struct {
    VGPU_HDR  hdr;
    VGPU_RECT r;
    UINT32    resource_id;
    UINT32    padding;
} VGPU_FLUSH;

/* VirtQueue structures — matches VirtIO 0.9 legacy layout */
typedef struct {
    UINT64 address;
    UINT32 length;
    UINT16 flags;
    UINT16 next;
} VGPU_DESC;

typedef struct {
    UINT16          flags;
    volatile UINT16 index;
    UINT16          ring[64];
} VGPU_AVAIL;

typedef struct { UINT32 id; UINT32 length; } VGPU_USED_ELEM;

typedef struct {
    UINT16          flags;
    volatile UINT16 index;
    VGPU_USED_ELEM  ring[64];
} VGPU_USED;

#pragma pack(pop)

/* ======================================================================
 * Static storage — aligned for VirtIO queue PFN submission
 * Queue layout: desc(64*16=1024) + avail(~140) + [pad to 4K] + used
 * Total ≤ 8192 bytes → safe with 8K buffer.
 * ====================================================================== */
__declspec(align(4096)) static UINT8 s_queue_mem[8192];

/* Per-command buffers (cache-line aligned to avoid false sharing) */
__declspec(align(64)) static VGPU_CREATE_2D     s_cmd_create;
__declspec(align(64)) static VGPU_ATTACH_BACKING s_cmd_attach;
__declspec(align(64)) static VGPU_SET_SCANOUT    s_cmd_scanout;
__declspec(align(64)) static VGPU_TRANSFER_2D    s_cmd_transfer;
__declspec(align(64)) static VGPU_FLUSH          s_cmd_flush;
__declspec(align(64)) static VGPU_HDR            s_response;

/* ---- Driver state ---- */
static UINT16   s_io_base;
static UINT16   s_qsz;        /* actual queue size reported by device */
static VGPU_DESC  *s_desc;
static VGPU_AVAIL *s_avail;
static VGPU_USED  *s_used;
static int      s_ready;
static UINT32   s_width;
static UINT32   s_height;
static UINT32   s_stride;     /* row stride in pixels */

extern void memory_fence(void);

/* ======================================================================
 * Internal helpers
 * ====================================================================== */
static void vgpu_zero(void *p, UINTN n)
{
    UINT8 *b = (UINT8 *)p;
    UINTN i;
    for (i = 0; i < n; i++) b[i] = 0;
}

static void vgpu_hdr_init(VGPU_HDR *h, UINT32 type)
{
    h->type     = type;
    h->flags    = 0;
    h->fence_id = 0;
    h->ctx_id   = 0;
    h->padding  = 0;
}

/*
 * Submit a two-descriptor transaction:
 *   desc[0]: cmd_buf  — host reads  (no WRITE flag)
 *   desc[1]: resp_buf — host writes (WRITE flag)
 * Waits up to ~10M iterations for the used ring to advance.
 * Returns 1 on success, 0 on timeout.
 */
static int vgpu_submit(void *cmd_buf, UINT32 cmd_len,
                        void *resp_buf, UINT32 resp_len)
{
    UINT16 prev_used = s_used->index;
    UINT16 avail_idx;

    /* Descriptor 0: command (host reads) */
    s_desc[0].address = (UINT64)(UINTN)cmd_buf;
    s_desc[0].length  = cmd_len;
    s_desc[0].flags   = VIRTQ_DESC_F_NEXT;
    s_desc[0].next    = 1;

    /* Descriptor 1: response (host writes) */
    s_desc[1].address = (UINT64)(UINTN)resp_buf;
    s_desc[1].length  = resp_len;
    s_desc[1].flags   = VIRTQ_DESC_F_WRITE;
    s_desc[1].next    = 0;

    /* Post descriptor chain to available ring */
    avail_idx = s_avail->index % s_qsz;
    s_avail->ring[avail_idx] = 0;
    memory_fence();
    s_avail->index++;
    memory_fence();

    /* Kick device */
    __outword(s_io_base + VIRTIO_PCI_QUEUE_NOTIFY, 0);

    /* Poll used ring (synchronous, no interrupts) */
    {
        UINT32 t;
        for (t = 0; t < 10000000u; t++) {
            memory_fence();
            if (s_used->index != prev_used) return 1;
        }
    }
    return 0;
}

/* ======================================================================
 * Public API
 * ====================================================================== */
int virtio_gpu_initialize(const ASAS_PCI_DEVICE *dev,
                           UINT32 *backbuf,
                           UINT32 width, UINT32 height, UINT32 stride)
{
    UINT32 bar_index;
    UINTN  avail_off, used_off;

    s_ready   = 0;
    s_io_base = 0;
    s_width   = width;
    s_height  = height;
    s_stride  = stride;

    /* ---- Find IO BAR ---- */
    for (bar_index = 0; bar_index < 6; bar_index++) {
        if ((dev->bars[bar_index] & 1u) != 0) {
            s_io_base = (UINT16)(dev->bars[bar_index] & ~3u);
            if (s_io_base == 0) {
                /* BAR not programmed — assign a base address */
                s_io_base = 0xC100;
                pci_write_bar(dev, bar_index, (UINT32)s_io_base | 1u);
            }
            break;
        }
    }
    if (s_io_base == 0) {
        logger_write("WARN", "VirtIO GPU: no IO BAR");
        return 0;
    }

    pci_enable_bus_mastering(dev);

    /* ---- VirtIO legacy init sequence ---- */
    __outbyte(s_io_base + VIRTIO_PCI_STATUS, 0);                            /* reset */
    __outbyte(s_io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    __outbyte(s_io_base + VIRTIO_PCI_STATUS,
              VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    (void)__indword(s_io_base + VIRTIO_PCI_HOST_FEATURES); /* read features */
    __outdword(s_io_base + VIRTIO_PCI_GUEST_FEATURES, 0);  /* no features needed */

    /* ---- Setup control queue (queue 0) ---- */
    __outword(s_io_base + VIRTIO_PCI_QUEUE_SELECT, 0);
    s_qsz = __inword(s_io_base + VIRTIO_PCI_QUEUE_NUM);
    if (s_qsz == 0 || s_qsz > 64) s_qsz = 64;

    vgpu_zero(s_queue_mem, sizeof(s_queue_mem));
    s_desc = (VGPU_DESC *)s_queue_mem;
    avail_off = sizeof(VGPU_DESC) * (UINTN)s_qsz;
    s_avail   = (VGPU_AVAIL *)(s_queue_mem + avail_off);
    used_off  = (avail_off + 6u + sizeof(UINT16) * (UINTN)s_qsz + 4095u) & ~(UINTN)4095u;
    s_used    = (VGPU_USED *)(s_queue_mem + used_off);

    __outdword(s_io_base + VIRTIO_PCI_QUEUE_PFN,
               (UINT32)((UINTN)s_queue_mem >> 12));
    __outbyte(s_io_base + VIRTIO_PCI_STATUS,
              VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    logger_write("INFO", "VirtIO GPU: queues ready");

    /* ======================================================
     * Step 1: RESOURCE_CREATE_2D — allocate GPU 2D resource
     * ====================================================== */
    vgpu_zero(&s_cmd_create, sizeof(s_cmd_create));
    vgpu_hdr_init(&s_cmd_create.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
    s_cmd_create.resource_id = 1;
    s_cmd_create.format      = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    s_cmd_create.width       = width;
    s_cmd_create.height      = height;

    vgpu_zero(&s_response, sizeof(s_response));
    if (!vgpu_submit(&s_cmd_create, sizeof(s_cmd_create),
                     &s_response, sizeof(s_response))) {
        logger_write("WARN", "VirtIO GPU: RESOURCE_CREATE_2D timeout");
        return 0;
    }
    logger_write("INFO", "VirtIO GPU: 2D resource created");

    /* ======================================================
     * Step 2: RESOURCE_ATTACH_BACKING — bind our backbuffer
     * The GPU will DMA-read from backbuf on TRANSFER_TO_HOST.
     * addr = guest physical address (identity-mapped in kernel).
     * ====================================================== */
    vgpu_zero(&s_cmd_attach, sizeof(s_cmd_attach));
    vgpu_hdr_init(&s_cmd_attach.hdr, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    s_cmd_attach.resource_id       = 1;
    s_cmd_attach.nr_entries        = 1;
    s_cmd_attach.entries[0].addr   = (UINT64)(UINTN)backbuf;
    s_cmd_attach.entries[0].length = stride * height * (UINT32)sizeof(UINT32);
    s_cmd_attach.entries[0].padding = 0;

    vgpu_zero(&s_response, sizeof(s_response));
    if (!vgpu_submit(&s_cmd_attach, sizeof(s_cmd_attach),
                     &s_response, sizeof(s_response))) {
        logger_write("WARN", "VirtIO GPU: RESOURCE_ATTACH_BACKING timeout");
        return 0;
    }
    logger_write("INFO", "VirtIO GPU: backbuffer backing attached");

    /* ======================================================
     * Step 3: SET_SCANOUT — connect resource to display
     * ====================================================== */
    vgpu_zero(&s_cmd_scanout, sizeof(s_cmd_scanout));
    vgpu_hdr_init(&s_cmd_scanout.hdr, VIRTIO_GPU_CMD_SET_SCANOUT);
    s_cmd_scanout.r.x         = 0;
    s_cmd_scanout.r.y         = 0;
    s_cmd_scanout.r.width     = width;
    s_cmd_scanout.r.height    = height;
    s_cmd_scanout.scanout_id  = 0;
    s_cmd_scanout.resource_id = 1;

    vgpu_zero(&s_response, sizeof(s_response));
    if (!vgpu_submit(&s_cmd_scanout, sizeof(s_cmd_scanout),
                     &s_response, sizeof(s_response))) {
        logger_write("WARN", "VirtIO GPU: SET_SCANOUT timeout");
        return 0;
    }
    logger_write("INFO", "VirtIO GPU: scanout configured — GPU path active");

    s_ready = 1;
    return 1;
}

void virtio_gpu_flush(UINT32 x, UINT32 y, UINT32 w, UINT32 h)
{
    if (!s_ready) return;

    /* TRANSFER_TO_HOST_2D: tell GPU to DMA-read dirty rect from backbuffer */
    vgpu_hdr_init(&s_cmd_transfer.hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    s_cmd_transfer.r.x         = x;
    s_cmd_transfer.r.y         = y;
    s_cmd_transfer.r.width     = w;
    s_cmd_transfer.r.height    = h;
    s_cmd_transfer.offset      = (UINT64)y * s_stride * sizeof(UINT32)
                                 + (UINT64)x * sizeof(UINT32);
    s_cmd_transfer.resource_id = 1;
    s_cmd_transfer.padding     = 0;

    s_response.type = 0;
    vgpu_submit(&s_cmd_transfer, sizeof(s_cmd_transfer),
                &s_response,     sizeof(s_response));

    /* RESOURCE_FLUSH: push GPU resource to physical display */
    vgpu_hdr_init(&s_cmd_flush.hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    s_cmd_flush.r.x         = x;
    s_cmd_flush.r.y         = y;
    s_cmd_flush.r.width     = w;
    s_cmd_flush.r.height    = h;
    s_cmd_flush.resource_id = 1;
    s_cmd_flush.padding     = 0;

    s_response.type = 0;
    vgpu_submit(&s_cmd_flush, sizeof(s_cmd_flush),
                &s_response,  sizeof(s_response));
}

int virtio_gpu_ready(void)
{
    return s_ready;
}
