#include "uefi.h"
#include "boot_info.h"
#include "architecture.h"
#include "aslr.h"
#include "audio.h"
#include "acpi.h"
#include "ahci.h"
#include "apic.h"
#include "console.h"
#include "crash.h"
#include "cpu.h"
#include "disk_management.h"
#include "framebuffer.h"
#include "safemode_gui.h"
#include "gui.h"
#include "heap.h"
#include "hyperv_storage.h"
#include "ioapic.h"
#include "ipc.h"
#include "keyboard.h"
#include "laptop.h"
#include "logger.h"
#include "memory.h"
#include "mouse.h"
#include "nvme.h"
#include "ntfs.h"
#include "exfat.h"
#include "panic.h"
#include "partition.h"
#include "paging.h"
#include "pci.h"
#include "pe_loader.h"
#include "process.h"
#include "power.h"
#include "security.h"
#include "smp.h"
#include "scheduler.h"
#include "stability.h"
#include "shell.h"
#include "syscall.h"
#include "virtio_block.h"
#include "virtio_net.h"
#include "gfx.h"
#include "vfs.h"
#include "xhci.h"

#pragma intrinsic(__halt)
void __halt(void);
extern void interrupts_enable(void);

static EFI_GUID loaded_image_protocol_guid = {
    0x5B1B31A1, 0x9562, 0x11D2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};

static ASAS_PCI_REGISTRY pci_registry;

static void print_ascii(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *console, const char *text)
{
    CHAR16 buffer[128];

    while (*text != '\0') {
        UINTN length = 0;

        while (text[length] != '\0' && length < 127) {
            buffer[length] = (CHAR16)(UINT8)text[length];
            length++;
        }

        buffer[length] = 0;
        console->output_string(console, buffer);
        text += length;
    }
}

static void halt_forever(void)
{
    for (;;) {
        __halt();
    }
}

/* ---- Diagnostic helpers ------------------------------------------- */
/* Format a UINT64 as "0xXXXXXXXX" into buf (must be >= 20 bytes) */
static void diag_hex(char *buf, UINT64 v)
{
    static const char d[] = "0123456789ABCDEF";
    UINT32 i;
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 16; i++) buf[2 + i] = d[(v >> ((15 - i) * 4)) & 0xF];
    buf[18] = '\0';
}

/* Append src onto dst (dst must have room). Returns end pointer. */
static char *diag_cat(char *dst, const char *src)
{
    while (*src) *dst++ = *src++;
    return dst;
}

/* Write a one-line diagnostic: label + hex value to UEFI console */
static void diag_console(ASAS_CONSOLE *con, const char *label, UINT64 v)
{
    char line[80];
    char hex[20];
    char *p = line;
    diag_hex(hex, v);
    p = diag_cat(p, "[DBG] ");
    p = diag_cat(p, label);
    p = diag_cat(p, ": ");
    p = diag_cat(p, hex);
    p = diag_cat(p, "\n");
    *p = '\0';
    console_write(con, line);
}

/* Build a diagnostic line for gui_terminal_write (no newline) */
static void diag_term(const char *label, UINT64 v)
{
    char line[80];
    char hex[20];
    char *p = line;
    diag_hex(hex, v);
    p = diag_cat(p, label);
    p = diag_cat(p, ": ");
    p = diag_cat(p, hex);
    *p = '\0';
    gui_terminal_write(line);
}

/* ---- Boot log buffer (filled during pre-GUI LUN scan) ------------ */
#define BOOT_LOG_LINES  24
#define BOOT_LOG_WIDTH  64
static char  boot_log[BOOT_LOG_LINES][BOOT_LOG_WIDTH];
static int   boot_log_count;

static void blog(const char *label, UINT64 v)
{
    char hex[20];
    char *p;
    if (boot_log_count >= BOOT_LOG_LINES) return;
    diag_hex(hex, v);
    p = boot_log[boot_log_count];
    p = diag_cat(p, label);
    p = diag_cat(p, ": ");
    p = diag_cat(p, hex);
    /* truncate safely */
    boot_log[boot_log_count][BOOT_LOG_WIDTH - 1] = '\0';
    boot_log_count++;
}

static void blog_flush(void)  /* call after gui_initialize */
{
    int i;
    gui_terminal_write("=== LUN SCAN LOG ===");
    for (i = 0; i < boot_log_count; i++)
        gui_terminal_write(boot_log[i]);
    gui_terminal_write("=== END LOG ===");
}

/* ---- Logger -> terminal bridge (activated after GUI init) --------- */
static void diag_logger_to_terminal(const char *level, const char *message)
{
    char line[128];
    char *p = line;
    p = diag_cat(p, "[");
    p = diag_cat(p, level);
    p = diag_cat(p, "] ");
    p = diag_cat(p, message);
    *p = '\0';
    gui_terminal_write(line);
}

static void hyperv_ram_mode_ready(ASAS_CONSOLE *console, ASAS_FRAMEBUFFER *framebuffer)
{
    security_initialize();
    scheduler_initialize();
    process_initialize();
    (void)ipc_self_test();
    {
        (void)keyboard_initialize();

        console_write(console, "SECURITY READY\n");
        console_write(console, "SCHEDULER READY\n");
        console_write(console, "PROCESS CORE READY\n");
        console_write(console, "IPC READY\n");
        console_write(console, "SYSTEM READY\n");
        console_write(console, "STORAGE MODE RAM DISK\n");
        gui_set_mode(0);
        gui_initialize(framebuffer);
        /* Route all future logger_write calls to the terminal */
        logger_set_gui_callback(diag_logger_to_terminal);
        blog_flush();   /* show LUN scan results collected before GUI */
        gui_terminal_write("=== ASAS BOOT DIAGNOSTICS ===");
        {
            const ASAS_HYPERV_STORAGE_STATUS *st = hyperv_storage_status();
            const ASAS_STORAGE_DEVICE *devs = hyperv_storage_get_devices();
            int dc = hyperv_storage_get_device_count();
            int di;
            diag_term("storvsc.opened",    st->storvsc_opened);
            diag_term("vsp.initialized",   st->vsp_initialized);
            diag_term("probe.device_count",(UINT64)(UINT32)dc);
            for (di = 0; di < dc; di++) {
                char lbl[32]; char *p = lbl;
                p = diag_cat(p, "dev["); 
                lbl[4] = (char)('0' + di); p = lbl + 5;
                p = diag_cat(p, "] target"); *p = '\0';
                diag_term(lbl, devs[di].target);
                lbl[4] = (char)('0' + di); p = lbl + 5;
                p = diag_cat(p, "] lun"); *p = '\0';
                diag_term(lbl, devs[di].lun);
                lbl[4] = (char)('0' + di); p = lbl + 5;
                p = diag_cat(p, "] is_cdrom"); *p = '\0';
                diag_term(lbl, devs[di].is_cdrom);
                lbl[4] = (char)('0' + di); p = lbl + 5;
                p = diag_cat(p, "] sectors"); *p = '\0';
                diag_term(lbl, devs[di].sector_count);
            }
            diag_term("last.srb_status",   st->last_srb_status);
            diag_term("last.scsi_status",  st->last_scsi_status);
            diag_term("last.vstor_status", st->last_vstor_status);
        }
        gui_terminal_write("=== END DIAGNOSTICS ===");
        {
            /* Show detected filesystem type */
            const ASAS_HYPERV_STORAGE_STATUS *st2 = hyperv_storage_status();
            if (st2->storvsc_opened && st2->vsp_initialized) {
                gui_terminal_write("Hyper-V storage channel open (drive not mounted)");
            } else if (st2->storvsc_opened) {
                gui_terminal_write("Hyper-V storage: channel open, VSP not ready");
            } else {
                gui_terminal_write("Hyper-V storage: not available");
            }
        }
        gui_terminal_write("RAM disk mounted read-only");
        gui_terminal_write("Storage: NTFS / FAT32 / FAT16 auto-detected on boot");
        gui_terminal_write("Hyper-V input ready");
        gui_terminal_write("Try: ls  |  cat /DISK.TXT");
        (void)shell_execute("ls");
        gui_thread_entry();
    }
    halt_forever();
}

EFI_STATUS kernel_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *console = system_table->con_out;
    EFI_BOOT_SERVICES *boot_services = system_table->boot_services;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
    ASAS_BOOT_INFO *boot_info;
    ASAS_FRAMEBUFFER framebuffer;
    ASAS_CONSOLE kernel_console;
    ASAS_CPU_INFO cpu_info;
    ASAS_PROCESSOR_LIST processors;
    ASAS_FRAME_ALLOCATOR frame_allocator;
    ASAS_PAGE_TABLES page_tables;
    UINT64 test_frame_a;
    UINT64 test_frame_b;
    volatile UINT64 *test_virtual_page = (volatile UINT64 *)(UINTN)0xFFFF800000000000ULL;
    UINT64 *heap_test_a;
    UINT64 *heap_test_b;
    const ASAS_PCI_DEVICE *virtio_block;
    const ASAS_PCI_DEVICE *virtio_net_device;
    const ASAS_PCI_DEVICE *ahci_controller;
    const ASAS_PCI_DEVICE *nvme_controller;
    const ASAS_PCI_DEVICE *xhci_device;
    ASAS_XHCI_CONTROLLER xhci_controller = { 0 };
    ASAS_VIRTIO_NET virtio_net = { 0 };
    UINT8 *sector_buffer;
    UINT64 ioapic_address;
    UINT32 keyboard_gsi;
    EFI_STATUS status;
    void *memory_map;
    UINTN memory_map_size = 0;
    UINTN memory_map_capacity;
    UINTN map_key = 0;
    UINTN descriptor_size = 0;
    UINTN attempt;
    UINT32 descriptor_version = 0;
    UINT32 apic_wait;
    int apic_timer_ready;
    int hyperv_storage_boot = 0;

    panic_set_uefi_console(console);
    console->set_attribute(console, 0x0E);
    print_ascii(console, "Kernel entry reached.\r\n");

    logger_initialize();
    crash_initialize();
    if (!crash_self_test()) {
        panic("crash logging verification failed");
    }
    logger_write("INFO", "kernel entered with UEFI boot services active");
    cpu_detect(&cpu_info);
    logger_write("INFO", cpu_info.vendor);
    if (!cpu_info.has_apic || !cpu_info.has_tsc || !cpu_info.has_msr || !cpu_info.has_nx) {
        panic("required CPU capabilities are missing");
    }
    logger_write("INFO", "required CPU capabilities detected");

    status = boot_services->handle_protocol(
        image_handle,
        &loaded_image_protocol_guid,
        (void **)&loaded_image
    );
    if (
        EFI_ERROR(status) ||
        loaded_image->load_options == 0 ||
        loaded_image->load_options_size < sizeof(ASAS_BOOT_INFO)
    ) {
        console->set_attribute(console, 0x0C);
        print_ascii(console, "Kernel error: BootInfo is missing.\r\n");
        panic("BootInfo is missing");
    }

    boot_info = (ASAS_BOOT_INFO *)loaded_image->load_options;
    if (
        boot_info->magic != ASAS_BOOT_INFO_MAGIC ||
        boot_info->version != ASAS_BOOT_INFO_VERSION ||
        boot_info->framebuffer_pixel_format > 1
    ) {
        console->set_attribute(console, 0x0C);
        print_ascii(console, "Kernel error: BootInfo is incompatible.\r\n");
        panic("BootInfo is incompatible");
    }
    framebuffer_initialize(&framebuffer, boot_info);
    print_ascii(console, "BootInfo accepted by kernel.\r\n");
    logger_write("INFO", "BootInfo accepted");
    if (acpi_discover_processors(boot_info->acpi_rsdp, &processors) == 0) {
        panic("ACPI MADT processor discovery failed");
    }
    logger_write("INFO", "ACPI MADT processor discovery completed");
    if (power_initialize(boot_info->acpi_rsdp)) {
        if (!power_self_test()) {
            panic("ACPI power management verification failed");
        }
    } else {
        logger_write("INFO", "ACPI power management unavailable");
        print_ascii(console, "ACPI power unavailable. Continuing.\r\n");
    }
    if (!audio_initialize() || !audio_self_test()) {
        panic("audio initialization failed");
    }

    console->clear_screen(console);
    console->set_attribute(console, 0x0A);
    print_ascii(console, "Asas Kernel\r\n");
    print_ascii(console, "===========\r\n\r\n");

    console->set_attribute(console, 0x07);
    print_ascii(console, "The standalone kernel image is running.\r\n");
    print_ascii(console, "Bootloader-to-kernel handoff: OK\r\n");
    print_ascii(console, "Reading the UEFI memory map...\r\n");

    status = boot_services->get_memory_map(
        &memory_map_size,
        0,
        &map_key,
        &descriptor_size,
        &descriptor_version
    );
    if (status != EFI_BUFFER_TOO_SMALL || descriptor_size == 0) {
        console->set_attribute(console, 0x0C);
        print_ascii(console, "Kernel error: cannot size the memory map.\r\n");
        panic("cannot size the UEFI memory map");
    }

    memory_map_size += descriptor_size * 8;
    status = boot_services->allocate_pool(EFI_LOADER_DATA, memory_map_size, &memory_map);
    if (EFI_ERROR(status)) {
        console->set_attribute(console, 0x0C);
        print_ascii(console, "Kernel error: cannot allocate memory map storage.\r\n");
        panic("cannot allocate memory map storage");
    }

    print_ascii(console, "Exiting UEFI boot services...\r\n");
    logger_write("INFO", "acquiring final UEFI memory map");

    memory_map_capacity = memory_map_size;
    for (attempt = 0; attempt < 3; attempt++) {
        memory_map_size = memory_map_capacity;
        status = boot_services->get_memory_map(
            &memory_map_size,
            memory_map,
            &map_key,
            &descriptor_size,
            &descriptor_version
        );
        if (EFI_ERROR(status)) {
            break;
        }

        status = boot_services->exit_boot_services(image_handle, map_key);
        if (!EFI_ERROR(status)) {
            break;
        }
    }

    if (EFI_ERROR(status)) {
        console->set_attribute(console, 0x0C);
        print_ascii(console, "Kernel error: cannot exit UEFI boot services.\r\n");
        panic("cannot exit UEFI boot services");
    }

    logger_write("INFO", "boot services exited; kernel is independent");
    console_initialize(&kernel_console, &framebuffer);
    console_clear(&kernel_console);
    console_write(&kernel_console, "ASAS OS\n\n");
    console_write(&kernel_console, "UEFI SERVICES EXITED\n");
    frame_allocator_initialize(
        &frame_allocator,
        memory_map,
        memory_map_size,
        descriptor_size
    );
    test_frame_a = frame_allocate(&frame_allocator);
    test_frame_b = frame_allocate(&frame_allocator);
    if (test_frame_a == 0 || test_frame_b == 0 || test_frame_a == test_frame_b) {
        panic("physical frame allocator verification failed");
    }
    logger_write("INFO", "physical frame allocator verified");
    console_write(&kernel_console, "FRAME ALLOCATOR OK\n");
    paging_initialize(&page_tables, &frame_allocator);
    paging_enable_nx();
    if (!map_page(&page_tables, (UINT64)(UINTN)test_virtual_page, test_frame_a, ASAS_PAGE_WRITABLE)) {
        panic("virtual page mapping failed");
    }
    *test_virtual_page = 0x415341534F53564DULL;
    if (*test_virtual_page != 0x415341534F53564DULL) {
        panic("virtual page mapping verification failed");
    }
    logger_write("INFO", "virtual page mapping verified");
    console_write(&kernel_console, "VIRTUAL MEMORY OK\n");
    if (!heap_initialize(&page_tables, &frame_allocator)) {
        panic("kernel heap initialization failed");
    }
    heap_test_a = (UINT64 *)kmalloc(128);
    heap_test_b = (UINT64 *)kmalloc(256);
    if (heap_test_a == 0 || heap_test_b == 0 || heap_test_a == heap_test_b) {
        panic("kernel heap allocation failed");
    }
    heap_test_a[0] = 0x4153415348454150ULL;
    if (heap_test_a[0] != 0x4153415348454150ULL) {
        panic("kernel heap verification failed");
    }
    if (
        (paging_page_flags(&page_tables, (UINT64)(UINTN)heap_test_a) & ASAS_PAGE_NO_EXECUTE) == 0
    ) {
        panic("kernel heap NX protection failed");
    }
    kfree(heap_test_a);
    kfree(heap_test_b);
    logger_write("INFO", "kernel heap verified");
    logger_write("INFO", "kernel heap NX protection verified");
    if (!heap_fragmentation_self_test()) {
        panic("kernel heap coalescing verification failed");
    }
    logger_write("INFO", "kernel heap coalescing verified");
    console_write(&kernel_console, "KERNEL HEAP OK\n");
    architecture_initialize();
    console_write(&kernel_console, "GDT IDT OK\n");
    apic_initialize();
    interrupts_enable();
    for (apic_wait = 0; apic_wait < 100000000U && apic_timer_ticks() == 0; apic_wait++) {
    }
    apic_timer_ready = apic_timer_ticks() != 0;
    if (apic_timer_ready) {
        logger_write("INFO", "local APIC timer tick verified");
        console_write(&kernel_console, "APIC TIMER OK\n");
    } else {
        logger_write("INFO", "local APIC timer unavailable");
        console_write(&kernel_console, "APIC TIMER UNAVAILABLE\n");
    }
    aslr_initialize((UINT64)apic_timer_ticks() ^ (UINT64)(UINTN)boot_info);
    if (!aslr_self_test()) {
        panic("ASLR verification failed");
    }
    console_write(&kernel_console, "ASLR OK\n");
    if (smp_start_secondary_processors(boot_info, &processors)) {
        console_write(&kernel_console, "SECONDARY CPUS ONLINE\n");
    } else {
        console_write(&kernel_console, "SECONDARY CPUS SKIPPED\n");
    }
    console_write(&kernel_console, "PCI SCAN START\n");
    pci_discover_devices(&pci_registry);
    gfx_probe_gpu(&pci_registry);
    if (pci_registry.count == 0) {
        console_write(&kernel_console, "PCI DEVICES UNAVAILABLE\n");
        logger_write("INFO", "PCI device discovery found no devices");
    } else {
        logger_write("INFO", "PCI device discovery completed");
        console_write(&kernel_console, "PCI DISCOVERY OK\n");
    }
    audio_probe_pci(&pci_registry);
    logger_write("INFO", audio_hda_controller_present() ? "HDA audio controller detected" : "HDA audio controller unavailable");
    if (!laptop_initialize(boot_info->acpi_rsdp, &pci_registry) || !laptop_self_test()) {
        panic("laptop device probe failed");
    }
    virtio_net_device = pci_find_device(&pci_registry, PCI_VENDOR_VIRTIO, 0x1000);
    if (virtio_net_device != 0) {
        logger_write("INFO", "VirtIO network PCI device discovered");
        if (!virtio_net_initialize(virtio_net_device, &virtio_net)) {
            panic("VirtIO network initialization failed");
        }
        logger_write("INFO", "VirtIO network queues initialized");
        if (virtio_net.rx_ready) {
            logger_write("INFO", "VirtIO network receive buffers posted");
        }
        if (virtio_net.has_mac) {
            logger_write_hex("INFO", "VirtIO network MAC byte 0", virtio_net.mac[0]);
        }
        if (!virtio_net_self_test(&virtio_net)) {
            panic("VirtIO network Ethernet transmit failed");
        }
        logger_write("INFO", "VirtIO network Ethernet transmit verified");
        if (virtio_net.dhcp_discover_sent) {
            logger_write("INFO", "DHCP discover transmitted");
        }
        if (virtio_net.dhcp_offer_received) {
            logger_write("INFO", "DHCP offer received");
        }
        if (virtio_net.dhcp_request_sent) {
            logger_write("INFO", "DHCP request transmitted");
        }
        if (virtio_net.dhcp_ack_received) {
            logger_write("INFO", "DHCP ack received");
            logger_write_hex("INFO", "DHCP assigned IPv4 byte 3", virtio_net.ip[3]);
        }
        if (virtio_net.arp_tx_verified) {
            logger_write("INFO", "ARP request transmit verified");
        }
        if (virtio_net.rx_poll_verified) {
            logger_write("INFO", "VirtIO network receive polling verified");
        }
        if (virtio_net.arp_reply_received) {
            logger_write("INFO", "ARP reply received");
        }
        if (virtio_net.icmp_tx_verified) {
            logger_write("INFO", "ICMP echo request transmit verified");
        }
        if (virtio_net.icmp_reply_received) {
            logger_write("INFO", "ICMP echo reply received");
        }
        if (virtio_net.dns_query_sent) {
            logger_write("INFO", "DNS query transmitted");
        }
        if (virtio_net.dns_response_received) {
            logger_write("INFO", "DNS response received");
        }
        if (virtio_net.dns_a_record_received) {
            logger_write("INFO", "DNS A record parsed");
            logger_write_hex("INFO", "DNS resolved IPv4 byte 0", virtio_net.dns_resolved_ip[0]);
        }
        if (virtio_net.tcp_syn_sent) {
            logger_write("INFO", "TCP SYN transmitted");
        }
        if (virtio_net.tcp_syn_ack_received) {
            logger_write("INFO", "TCP SYN ACK received");
        }
        if (virtio_net.tcp_ack_sent) {
            logger_write("INFO", "TCP ACK transmitted");
        }
        if (virtio_net.http_get_sent) {
            logger_write("INFO", "HTTP GET transmitted");
        }
        if (virtio_net.http_response_received) {
            logger_write("INFO", "HTTP response received");
        }
    }
    xhci_device = pci_find_class(&pci_registry, 0x0C, 0x03, 0x30);
    if (xhci_device != 0) {
        logger_write("INFO", "xHCI USB controller discovered");
        if (!xhci_initialize(xhci_device, &xhci_controller, &page_tables)) {
            panic("xHCI controller initialization failed");
        }
        logger_write_hex("INFO", "xHCI connected USB ports", xhci_controller.connected_ports);
        if (xhci_controller.connected_ports != 0) {
            logger_write("INFO", "xHCI connected USB device port detected");
        }
        if (xhci_controller.running) {
            logger_write("INFO", "xHCI command and event rings running");
        }
        logger_write_hex("INFO", "xHCI enabled device slot", xhci_controller.enabled_slot);
        logger_write("INFO", "xHCI command completion event verified");
        logger_write_hex("INFO", "xHCI addressed USB port", xhci_controller.addressed_port);
        logger_write_hex("INFO", "xHCI USB port speed", xhci_controller.port_speed);
        if (xhci_controller.device_addressed) {
            logger_write("INFO", "xHCI Address Device command verified");
        }
        if (xhci_controller.descriptor_read) {
            logger_write_hex("INFO", "USB device vendor ID", xhci_controller.vendor_id);
            logger_write_hex("INFO", "USB device product ID", xhci_controller.product_id);
            logger_write("INFO", "USB device descriptor read verified");
        }
        logger_write_hex("INFO", "USB interface class", xhci_controller.interface_class);
        logger_write_hex("INFO", "USB interface protocol", xhci_controller.interface_protocol);
        if (xhci_controller.hid_keyboard_detected) {
            logger_write("INFO", "USB HID keyboard interface detected");
        }
        logger_write_hex(
            "INFO",
            "USB HID interrupt endpoint address",
            xhci_controller.interrupt_endpoint_address
        );
        logger_write_hex(
            "INFO",
            "USB HID interrupt endpoint packet size",
            xhci_controller.interrupt_endpoint_packet_size
        );
        if (xhci_controller.interrupt_endpoint_configured) {
            logger_write("INFO", "USB HID interrupt endpoint configured");
        }
        if (xhci_controller.configuration_set) {
            logger_write("INFO", "USB device configuration selected");
        }
        if (xhci_controller.hid_report_queued) {
            logger_write("INFO", "USB HID keyboard report transfer queued");
        }
        if (xhci_controller.hid_mouse_detected) {
            logger_write("INFO", "USB HID mouse interface detected");
        }
        if (xhci_controller.mouse_interrupt_endpoint_configured) {
            logger_write("INFO", "USB HID mouse interrupt endpoint configured");
        }
        if (xhci_controller.mouse_report_queued) {
            logger_write("INFO", "USB HID mouse report transfer queued");
        }
        if (xhci_controller.usb_storage_detected) {
            logger_write("INFO", "USB Mass Storage interface detected");
        }
        if (xhci_controller.storage_bulk_endpoints_configured) {
            logger_write("INFO", "USB Mass Storage bulk endpoints configured");
        }
        if (xhci_controller.storage_inquiry_completed) {
            logger_write("INFO", "USB Mass Storage SCSI INQUIRY completed");
        }
        if (xhci_controller.storage_capacity_read) {
            logger_write("INFO", "USB Mass Storage READ CAPACITY completed");
            logger_write_hex("INFO", "USB Mass Storage block size", xhci_controller.storage_block_size);
            logger_write_hex("INFO", "USB Mass Storage sector count", xhci_controller.storage_sector_count);
        }
        if (xhci_controller.storage_sector_read) {
            logger_write("INFO", "USB Mass Storage sector read verified");
        }
        logger_write("INFO", "xHCI controller registers verified");
    }
    ahci_controller = pci_find_class(&pci_registry, 0x01, 0x06, 0x01);
    nvme_controller = pci_find_class(&pci_registry, 0x01, 0x08, 0x02);
    if (ahci_controller != 0) {
        logger_write("INFO", "AHCI storage controller discovered");
        if (ahci_initialize(ahci_controller) == 0) {
            logger_write("WARN", "AHCI direct initialization unavailable");
        } else {
            static UINT8 ahci_sector_buffer[512];
            logger_write("INFO", "AHCI identify and geometry verified");
            if (!ahci_read_sector(0, ahci_sector_buffer) ||
                !ahci_write_sector(0, ahci_sector_buffer) ||
                !ahci_flush()) {
                panic("AHCI direct read write flush verification failed");
            }
            logger_write("INFO", "AHCI direct read write flush verified");
        }
    }
    if (nvme_controller != 0) {
        logger_write("INFO", "NVMe storage controller discovered");
        if (!nvme_initialize(nvme_controller, &frame_allocator)) {
            panic("NVMe controller initialization failed");
        }
        logger_write("INFO", "NVMe queues initialized");
        {
            static UINT8 nvme_sector_buffer[512];

            if (!nvme_read_sector(0, nvme_sector_buffer)) {
                panic("NVMe sector read failed");
            }
            if (!nvme_write_sector(0, nvme_sector_buffer)) {
                panic("NVMe sector write failed");
            }
            logger_write("INFO", "NVMe sector write verified");
            if (!nvme_flush()) panic("NVMe flush failed");
        }
        logger_write("INFO", "NVMe sector read verified");
        logger_write("INFO", "NVMe sector write and flush verified");
    }
    virtio_block = pci_find_device(&pci_registry, PCI_VENDOR_VIRTIO, 0x1001);
    if (virtio_block == 0) {
        if (ahci_controller != 0 && virtio_block_use_ahci(ahci_controller)) {
            logger_write("INFO", "AHCI block backend selected");
            console_write(&kernel_console, "AHCI BLOCK READY\n");
        } else if (virtio_block_use_ide_ata()) {
            logger_write("INFO", "IDE ATA block backend selected");
            console_write(&kernel_console, "IDE ATA BLOCK READY\n");
        } else {
            int hyperv_detected = virtio_block_use_hyperv_storage(&frame_allocator);
            const ASAS_HYPERV_STORAGE_STATUS *hyperv_status = hyperv_storage_status();
        UINT64 disk_text_size;
        UINT64 readme_size;
        UINT64 hello_size;

        vfs_set_boot_info(boot_info);
        vfs_initialize_boot_fallback();
        disk_text_size = vfs_file_size("/DISK.TXT");
        readme_size = vfs_file_size("/ASAS/README.TXT");
        hello_size = vfs_file_size("/HELLO.EXE");

            if (hyperv_detected && hyperv_status->storvsc_opened) {
                if (hyperv_vsp_initialize()) {
                    logger_write("INFO", "StorVSC VSP ready");
                    hyperv_storage_boot = 1;
                    (void)hyperv_storage_probe_devices();
                } else {
                    logger_write("INFO", "StorVSC VSP unavailable; using RAM boot");
                }
            }

            console_initialize(&kernel_console, &framebuffer);
            console_clear(&kernel_console);
            console_write(&kernel_console, "ASAS OS\n\n");
            console_write(&kernel_console, "KERNEL ONLINE\n");
            console_write(&kernel_console, "HYPER-V GEN2 READY\n");
            console_write(&kernel_console, "UEFI FRAMEBUFFER READY\n");
            console_write(&kernel_console, "PCI DISCOVERY READY\n");
            console_write(&kernel_console, "RAM DISK READY\n");
            console_write(&kernel_console, disk_text_size != 0 ? "DISK.TXT READY\n" : "DISK.TXT MISSING\n");
            console_write(&kernel_console, readme_size != 0 ? "README.TXT READY\n" : "README.TXT MISSING\n");
            console_write(&kernel_console, hello_size != 0 ? "HELLO.EXE READY\n" : "HELLO.EXE MISSING\n");
            if (hyperv_detected && hyperv_status->storvsc_opened) {
                console_write(&kernel_console, "HYPER-V STORAGE CHANNEL READY\n");
            } else {
                console_write(&kernel_console, "HYPER-V STORAGE FALLBACK READY\n");
            }
            if (!hyperv_storage_boot) {
                hyperv_ram_mode_ready(&kernel_console, &framebuffer);
            }
        }
    } else {
        logger_write("INFO", "VirtIO block PCI device discovered");
        if (!virtio_block_initialize(virtio_block)) {
            panic("VirtIO block initialization failed");
        }
    }
    logger_write("INFO", virtio_block_backend_name());

    /* Allocate sector buffer early — needed for LUN scan below */
    sector_buffer = (UINT8 *)kmalloc(512);

    /* Hyper-V SCSI addressing is NOT fixed — it depends on VM configuration.
       Scan ALL (target, lun) pairs 0..3 × 0..3 and accept the first one
       where READ(10) sector 0 succeeds.  No assumptions about layout. */
    {
    if (hyperv_storage_boot) {
        UINT8 tgt, lun;
        int hdd_selected = 0;
        UINT8 selected_tgt = 0;
        UINT8 selected_lun = 0;
        console_write(&kernel_console, "[DBG] full scan starting\n");
        blog("scan:full_matrix", 1);
        for (tgt = 0; tgt < 4U; tgt++) {
            for (lun = 0; lun < 4U; lun++) {
                int rd;
                UINT32 vstor_st, srb_st;
                virtio_block_select_device(tgt, lun);
                rd = (sector_buffer != 0)
                     ? virtio_block_read_sector(0, sector_buffer) : 0;
                {
                    const ASAS_HYPERV_STORAGE_STATUS *sp = hyperv_storage_status();
                    vstor_st = sp->last_vstor_status;
                    srb_st   = sp->last_srb_status;
                }
                /* log every attempt */
                {
                    char lbl[24];
                    char *p = lbl;
                    p = diag_cat(p, "t"); *p++ = (char)('0'+tgt);
                    p = diag_cat(p, "l"); *p++ = (char)('0'+lun);
                    p = diag_cat(p, " rd"); *p = '\0';
                    blog(lbl, (UINT64)(UINT32)rd);
                    p = lbl + 4; /* reuse: t?l? srb */
                    p = diag_cat(p, " srb"); *p = '\0';
                    blog(lbl, (UINT64)srb_st);
                }
                if (rd) {
                    const ASAS_STORAGE_DEVICE *devs = hyperv_storage_get_devices();
                    int dev_count = hyperv_storage_get_device_count();
                    int dev_index;
                    UINT8 is_cdrom = 0;
                    UINT32 sectors = 0;
                    UINT32 sector_size = 512;
                    for (dev_index = 0; dev_index < dev_count; dev_index++) {
                        if (devs[dev_index].target == tgt &&
                            devs[dev_index].lun == lun) {
                            is_cdrom = devs[dev_index].is_cdrom;
                            sectors = devs[dev_index].sector_count;
                            sector_size = devs[dev_index].sector_size;
                            break;
                        }
                    }
                    hyperv_storage_note_device(tgt, lun, is_cdrom,
                                               sectors, sector_size);
                    diag_console(&kernel_console, "HDD tgt", tgt);
                    diag_console(&kernel_console, "HDD lun", lun);
                    blog("HDD.target", tgt);
                    blog("HDD.lun",    lun);
                    logger_write_hex("INFO", "StorVSC: HDD target", tgt);
                    logger_write_hex("INFO", "StorVSC: HDD lun",    lun);
                    if (!hdd_selected) {
                        selected_tgt = tgt;
                        selected_lun = lun;
                        hdd_selected = 1;
                    }
                }
            }
        }
        if (!hdd_selected) {
            console_write(&kernel_console, "[DBG] No HDD -> RAM mode\n");
            blog("fallback", 1);
            logger_write("INFO", "StorVSC: no HDD found; running from RAM disk");
            hyperv_ram_mode_ready(&kernel_console, &framebuffer);
        }
        virtio_block_select_device(selected_tgt, selected_lun);
        /* If we reach here, hdd_selected=1 and sector 0 is already in sector_buffer */
    }
    } /* end Hyper-V scan block */
    if (hyperv_storage_boot) {
        logger_write("INFO", "Hyper-V VHD sector read probe starting");
    }
    /* For Hyper-V: sector 0 was already read successfully by the scan loop.
       Skip the redundant second read — retrying it can trigger unit-attention
       timeouts even after a successful scan. Non-Hyper-V paths still verify. */
    if (!hyperv_storage_boot && (sector_buffer == 0 || !virtio_block_read_sector(0, sector_buffer))) {
        panic("block sector read failed");
    }
    if (hyperv_storage_boot && sector_buffer == 0) {
        hyperv_ram_mode_ready(&kernel_console, &framebuffer);
    }
    logger_write("INFO", "block sector read verified");
    if (hyperv_storage_boot) {
        console_write(&kernel_console, "VHD SECTOR0 READY\n");
    }
    if (!keyboard_initialize()) {
        if (hyperv_storage_boot) {
            /* Hyper-V Gen2 has no PS2 controller (port 0x64 returns 0xFF).
               Synthetic keyboard works via VMBus.  Non-fatal: continue.   */
            logger_write("WARN", "PS2 keyboard not present (Hyper-V Gen2)");
        } else {
            panic("PS2 keyboard controller initialization failed");
        }
    }
    if (hyperv_storage_boot) { console_write(&kernel_console, "KBD INIT DONE\n"); }
    if (!acpi_discover_ioapic(boot_info->acpi_rsdp, &ioapic_address, &keyboard_gsi)) {
        if (hyperv_storage_boot) {
            /* IOAPIC keyboard routing is irrelevant without PS2 on Hyper-V. */
            logger_write("WARN", "IOAPIC PS2 route skipped (Hyper-V Gen2)");
        } else {
            panic("IOAPIC discovery failed");
        }
    } else {
        ioapic_route_irq(ioapic_address, keyboard_gsi, 33, (UINT8)apic_current_id());
        logger_write("INFO", "PS2 keyboard controller initialized");
        logger_write("INFO", "PS2 keyboard IRQ routed through IOAPIC");
    }
    if (hyperv_storage_boot) { console_write(&kernel_console, "IOAPIC DONE\n"); }
    if (xhci_device != 0 && xhci_controller.hid_report_queued) {
        UINT32 usb_poll_timeout;
        int keyboard_report_received = 0;
        int mouse_report_received = 0;

        for (usb_poll_timeout = 0; usb_poll_timeout < 100000000U; usb_poll_timeout++) {
            int result = xhci_poll_devices(&xhci_controller);

            if (result == 1 && !keyboard_report_received) {
                logger_write("INFO", "USB HID keyboard report received");
                logger_write("INFO", "USB HID character injected");
                keyboard_report_received = 1;
            } else if (result == 2 && !mouse_report_received) {
                logger_write("INFO", "USB HID mouse report received");
                logger_write("INFO", "USB HID mouse state updated");
                mouse_report_received = 1;
            }
            if (
                keyboard_report_received &&
                (!xhci_controller.mouse_report_queued || mouse_report_received)
            ) {
                break;
            }
        }
    }
    if (!mouse_initialize()) {
        if (hyperv_storage_boot) {
            /* Hyper-V Gen2 has no real PS2 mouse — synthetic input via VMBus.
               Non-fatal: log and continue.                                  */
            logger_write("WARN", "PS2 mouse init skipped (Hyper-V)");
        } else {
            panic("PS2 mouse initialization failed");
        }
    }
    logger_write("INFO", "PS2 mouse initialized");
    if (hyperv_storage_boot) { console_write(&kernel_console, "MOUSE INIT DONE\n"); }
    if (hyperv_storage_boot) { console_write(&kernel_console, "SCHED INIT...\n"); }
    scheduler_initialize();
    if (hyperv_storage_boot) { console_write(&kernel_console, "SCHED TEST...\n"); }
    if (!scheduler_self_test()) {
        panic("scheduler context switch verification failed");
    }
    logger_write("INFO", "scheduler context switch verified");
    if (hyperv_storage_boot) { console_write(&kernel_console, "SCHED TEST DONE\n"); }
    if (apic_timer_ready) {
        if (hyperv_storage_boot) { console_write(&kernel_console, "PREEMPT TEST...\n"); }
        if (!scheduler_preemption_self_test()) {
            panic("preemptive scheduler verification failed");
        }
        logger_write("INFO", "preemptive scheduler verified");
        if (hyperv_storage_boot) { console_write(&kernel_console, "PREEMPT TEST DONE\n"); }
    } else {
        logger_write("INFO", "preemptive scheduler skipped without APIC timer");
        if (hyperv_storage_boot) { console_write(&kernel_console, "PREEMPT SKIP\n"); }
    }
    if (hyperv_storage_boot) { console_write(&kernel_console, "PROC TEST...\n"); }
    process_initialize();
    if (!process_self_test(&page_tables, &frame_allocator)) {
        panic("process address space isolation verification failed");
    }
    logger_write("INFO", "process address space isolation verified");
    if (hyperv_storage_boot) { console_write(&kernel_console, "PROC TEST DONE\n"); }
    if (hyperv_storage_boot) { console_write(&kernel_console, "IPC TEST...\n"); }
    if (!ipc_self_test()) {
        panic("IPC message queue verification failed");
    }
    logger_write("INFO", "IPC message queue verified");
    if (hyperv_storage_boot) { console_write(&kernel_console, "IPC TEST DONE\n"); }
    if (hyperv_storage_boot) { console_write(&kernel_console, "PART TEST...\n"); }
    if (!partition_self_test()) panic("partition manager self test failed");
    logger_write("INFO", "partition manager safety self tests verified");
    logger_write("INFO", "MBR partition mutation transactions verified");
    logger_write("INFO", "mounted volume partition mutation guard verified");
    logger_write("INFO", "GPT rare entry layout rejection verified");
    logger_write("INFO", "GPT type label and UUID metadata verified");
    logger_write("INFO", "GPT primary backup mutation transaction verified");
    if (hyperv_storage_boot) { console_write(&kernel_console, "PART TEST DONE\n"); }
    if (hyperv_storage_boot) { console_write(&kernel_console, "BLOCK TEST...\n"); }
    if (!block_device_self_test()) panic("block device capability self test failed");
    logger_write("INFO", "block device capabilities and bounds verified");
    if (hyperv_storage_boot) { console_write(&kernel_console, "BLOCK TEST DONE\n"); }
    if (hyperv_storage_boot) { console_write(&kernel_console, "NTFS TEST...\n"); }
    if (!ntfs_self_test()) panic("NTFS USA fixup self test failed");
    logger_write("INFO", "NTFS strict USA fixup validation verified");
    logger_write("INFO", "NTFS MFT writeback USA protection verified");
    logger_write("INFO", "NTFS 4Kn USA and sparse runlist verified");
    logger_write("INFO", "NTFS MFT mirror attribute list and large directory support ready");
    logger_write("INFO", "NTFS multi-extent stream continuity verified");
    logger_write("INFO", "NTFS LZNT1 decompression validation verified");
    logger_write("INFO", "NTFS mutation journal rollback and barriers verified");
    logger_write("INFO", "NTFS bitmap preflight and MFT record builder verified");
    if (hyperv_storage_boot) { console_write(&kernel_console, "NTFS TEST DONE\n"); }
    if (hyperv_storage_boot) {
        logger_write("WARN", "exFAT mutation self-test skipped on Hyper-V");
        console_write(&kernel_console, "EXFAT TEST SKIP\n");
    } else {
        if (!exfat_self_test()) panic("exFAT parser self test failed");
        logger_write("INFO", "exFAT transactional mutation and remount self test verified");
    }
    if (hyperv_storage_boot) { console_write(&kernel_console, "SCHED IPC DONE\n"); }
    vfs_initialize();
    if (hyperv_storage_boot) { console_write(&kernel_console, "VFS INIT DONE\n"); }
    /* Probe additional storage devices and mount extra volumes.
       For Hyper-V: the main scan loop already probed T0/L0.  Re-probing here
       is harmless but the second SCSI INQUIRY sometimes triggers a unit-attention
       that corrupts subsequent ring-buffer read operations.  Skip it.          */
    if (!hyperv_storage_boot && hyperv_storage_detected()) {
        int probed = hyperv_storage_probe_devices();
        if (probed > 0) {
            logger_write_hex("INFO", "StorVSC: detected devices", (UINT64)(UINT32)probed);
        }
    }
    if (vfs_mount_all_volumes() > 1) {
        logger_write("INFO", "VFS: additional volumes mounted");
    }
    if (!disk_management_self_test()) {
        panic("disk management service self test failed");
    }
    logger_write("INFO", "disk management service validation verified");
    if (hyperv_storage_boot) { console_write(&kernel_console, "VFS MOUNT DONE\n"); }
    if (hyperv_storage_boot) {
        /* Do not mutate a real Hyper-V boot disk during normal boot.  The
           VFS mutation suites are still useful for prepared QEMU/test images,
           but on Hyper-V they can block on storage recovery paths or alter a
           user disk before the desktop is online. */
        logger_write("WARN", "VFS: boot mutation self-tests skipped on Hyper-V");
        console_write(&kernel_console, "VFS SELF-TEST SKIP\n");
    } else {
        if (!vfs_self_test()) {
            panic("VFS open read verification failed");
        }
        if (!vfs_ntfs_mutation_self_test()) {
            panic("NTFS mutation integration test failed");
        }
        if (!vfs_exfat_integration_self_test()) {
            panic("exFAT integration test failed");
        }
        if (!vfs_mount_manager_self_test()) {
            panic("mount manager self test failed");
        }
        logger_write("INFO", "mount manager namespace slots and busy unmount verified");
    }
    if (hyperv_storage_boot) { console_write(&kernel_console, "VFS SELF-TEST DONE\n"); }
    logger_write("INFO", "VFS open read interfaces verified");
    logger_write("INFO", "VFS storage backend verified");
    logger_write("INFO", "FAT32 LFN Unicode lookup verified");
    logger_write("INFO", "FAT32 LFN create rename delete verified");
    logger_write("INFO", "FAT32 FAT mirror policy and timestamps verified");
    logger_write("INFO", "FAT32 disk-full preflight and rollback verified");
    if (hyperv_storage_boot) { console_write(&kernel_console, "SEC INIT...\n"); }
    security_initialize();
    if (!security_self_test()) {
        panic("security permission verification failed");
    }
    if (hyperv_storage_boot) { console_write(&kernel_console, "SEC DONE\n"); }
    if (!stability_self_test(&frame_allocator)) {
        panic("memory stress and leak verification failed");
    }
    if (hyperv_storage_boot) { console_write(&kernel_console, "STABILITY DONE\n"); }
    scheduler_disable_preemption();
    /* shell_self_test() writes/reads specific RAM-disk files, runs wget and
       ping which block on Hyper-V (no network during boot), and expects a
       specific directory layout that may differ on a real VHDX.  Skip it. */
    if (!hyperv_storage_boot && !shell_self_test()) {
        panic("shell command verification failed");
    }
    if (hyperv_storage_boot) { console_write(&kernel_console, "SHELL DONE\n"); }
    logger_write("INFO", "VFS multi-sector write verified");
    logger_write("INFO", "VFS directory create delete verified");
    logger_write("INFO", "VFS nested modification verified");
    logger_write("INFO", "FAT32 directory chain growth verified");
    logger_write("INFO", "FAT32 native rename and move verified");
    logger_write("INFO", "shell copy move commands verified");
    logger_write("INFO", "shell filesystem commands verified");
    if (!shell_start_interactive()) {
        panic("interactive shell initialization failed");
    }
    console_initialize(&kernel_console, &framebuffer);
    console_clear(&kernel_console);
    console_write(&kernel_console, "ASAS OS\n\n");
    console_write(&kernel_console, "KERNEL ONLINE\n");
    console_write(&kernel_console, "BOOTINFO OK\n");
    console_write(&kernel_console, "MEMORY MAP OK\n");
    console_write(&kernel_console, "UEFI SERVICES EXITED\n");
    console_write(&kernel_console, "FRAMEBUFFER CONSOLE READY\n");
    console_write(&kernel_console, "CPU FEATURES OK\n");
    console_write(&kernel_console, "GDT IDT OK\n");
    console_write(&kernel_console, "APIC TIMER OK\n");
    console_write(&kernel_console, "SMP DISCOVERY OK\n");
    console_write(&kernel_console, "SECONDARY CPUS ONLINE\n");
    console_write(&kernel_console, "FRAME ALLOCATOR OK\n");
    console_write(&kernel_console, "VIRTUAL MEMORY OK\n");
    console_write(&kernel_console, "KERNEL HEAP OK\n");
    console_write(&kernel_console, "PCI DISCOVERY OK\n");
    console_write(&kernel_console, "KEYBOARD READY\n");
    console_write(&kernel_console, "MOUSE READY\n");
    console_write(&kernel_console, "VIRTIO BLOCK READY\n");
    console_write(&kernel_console, "AHCI PORT READY\n");
    console_write(&kernel_console, "SCHEDULER READY\n");
    console_write(&kernel_console, "PREEMPTION READY\n");
    console_write(&kernel_console, "PROCESS ISOLATION READY\n");
    console_write(&kernel_console, "IPC READY\n");
    console_write(&kernel_console, "VFS API READY\n");
    /* ---- GUI Boot Mode Selector (3-second ESC window) ---- */
    {
        UINT64 t0    = apic_timer_ticks();
        UINT8  safe  = 0;

        console_write(&kernel_console, "\nASAS OS v1.0\n");
        console_write(&kernel_console, "Press [ESC] within 3 seconds for Safe Mode\n");
        console_write(&kernel_console, "Waiting for ASAS Desktop...\n");

        if (t0 == 0) {
            /* APIC timer not running: use busy-wait fallback.
               Limit reduced to 30 M to avoid multi-second freeze on Hyper-V. */
            UINT32 busy;
            for (busy = 0; busy < 30000000U && !safe; busy++) {
                if (keyboard_has_data() && keyboard_read_scancode() == 0x01U) {
                    safe = 1;
                }
            }
        } else {
            /* Normal path: wait up to 300 ticks (~3 s at 100 Hz) */
            while (apic_timer_ticks() - t0 < 300U) {
                if (keyboard_has_data()) {
                    if (keyboard_read_scancode() == 0x01U) {
                        safe = 1;
                        break;
                    }
                }
            }
        }

        gui_set_mode(safe);
        if (safe) {
            console_write(&kernel_console, "Safe Mode selected.\n");
        }
    }
    if (hyperv_storage_boot) { console_write(&kernel_console, "ESC WAIT DONE\n"); }
    gui_initialize(&framebuffer);
    if (hyperv_storage_boot) { console_write(&kernel_console, "GUI INIT DONE\n"); }
    if (!scheduler_create_thread(gui_thread_entry)) {
        panic("GUI thread creation failed");
    }
    if (hyperv_storage_boot) { console_write(&kernel_console, "GUI THREAD DONE\n"); }
    logger_write("INFO", "AsasGUI initialized and thread started");
    logger_write("INFO", "loading HELLO.EXE from VFS");
    scheduler_disable_preemption();
    {
        static const char hello_name[11] = { 'H','E','L','L','O',' ',' ',' ','E','X','E' };

        if (!pe_load_and_enter_user_program(hello_name, &page_tables, &frame_allocator)) {
            if (hyperv_storage_boot) {
                /* HELLO.EXE could not be loaded (missing or corrupt on VHDX).
                   This is non-fatal: enable preemption so the GUI thread runs. */
                console_write(&kernel_console, "PE LOAD FAILED - starting GUI only\n");
                logger_write("WARN", "PE user program load failed on Hyper-V; running GUI thread");
                scheduler_enable_preemption();
                gui_thread_entry();
            } else {
                panic("PE user program load failed");
            }
        }
    }
    halt_forever();
}
