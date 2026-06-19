#include "uefi.h"
#include "boot_info.h"

#pragma intrinsic(__halt)
void __halt(void);

static EFI_GUID loaded_image_protocol_guid = {
    0x5B1B31A1, 0x9562, 0x11D2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};

static EFI_GUID simple_file_system_protocol_guid = {
    0x964E5B22, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};

static EFI_GUID graphics_output_protocol_guid = {
    0x9042A9DE, 0x23DC, 0x4A38, { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A }
};

static EFI_GUID acpi_20_table_guid = {
    0x8868E871, 0xE4F1, 0x11D3, { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 }
};

static int guid_equals(const EFI_GUID *left, const EFI_GUID *right)
{
    UINT32 index;

    if (left->data1 != right->data1 || left->data2 != right->data2 || left->data3 != right->data3) {
        return 0;
    }

    for (index = 0; index < 8; index++) {
        if (left->data4[index] != right->data4[index]) {
            return 0;
        }
    }

    return 1;
}

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

static void halt_with_error(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *console, const char *message)
{
    console->set_attribute(console, 0x0C);
    print_ascii(console, "Boot error: ");
    print_ascii(console, message);
    print_ascii(console, "\r\n");

    for (;;) {
        __halt();
    }
}

static int read_file_to_pool(
    EFI_BOOT_SERVICES *boot_services,
    EFI_FILE_PROTOCOL *root,
    CHAR16 *path,
    UINTN capacity,
    void **buffer,
    UINTN *size
)
{
    EFI_FILE_PROTOCOL *file;
    EFI_STATUS status;

    status = root->open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        return 0;
    }

    status = boot_services->allocate_pool(EFI_LOADER_DATA, capacity, buffer);
    if (EFI_ERROR(status)) {
        file->close(file);
        return 0;
    }

    *size = capacity;
    status = file->read(file, size, *buffer);
    file->close(file);
    return !EFI_ERROR(status);
}

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *console = system_table->con_out;
    EFI_BOOT_SERVICES *boot_services = system_table->boot_services;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *file_system;
    EFI_FILE_PROTOCOL *root;
    EFI_FILE_PROTOCOL *kernel_file;
    EFI_FILE_PROTOCOL *trampoline_file;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *graphics;
    EFI_LOADED_IMAGE_PROTOCOL *kernel_image;
    ASAS_BOOT_INFO *boot_info;
    EFI_HANDLE kernel_handle;
    EFI_STATUS status;
    void *kernel_buffer;
    UINTN kernel_size = 4 * 1024 * 1024;
    UINTN trampoline_size = 4096;
    void *disk_text_buffer = 0;
    void *readme_buffer = 0;
    void *user_program_buffer = 0;
    UINTN disk_text_size = 512;
    UINTN readme_size = 512;
    UINTN user_program_size = 65536;
    UINT64 trampoline_address = 0x8000;
    UINTN table_index;
    CHAR16 kernel_path[] = { '\\', 'A', 'S', 'A', 'S', '\\', 'K', 'E', 'R', 'N', 'E', 'L', '.', 'E', 'F', 'I', 0 };
    CHAR16 trampoline_path[] = { '\\', 'A', 'S', 'A', 'S', '\\', 'A', 'P', 'B', 'O', 'O', 'T', '.', 'B', 'I', 'N', 0 };
    CHAR16 disk_text_path[] = { '\\', 'D', 'I', 'S', 'K', '.', 'T', 'X', 'T', 0 };
    CHAR16 readme_path[] = { '\\', 'A', 'S', 'A', 'S', '\\', 'R', 'E', 'A', 'D', 'M', 'E', '.', 'T', 'X', 'T', 0 };
    CHAR16 user_program_path[] = { '\\', 'H', 'E', 'L', 'L', 'O', '.', 'E', 'X', 'E', 0 };

    console->clear_screen(console);
    console->set_attribute(console, 0x0B);
    print_ascii(console, "Asas OS Bootloader\r\n");
    print_ascii(console, "==================\r\n\r\n");
    console->set_attribute(console, 0x07);
    print_ascii(console, "Loading \\ASAS\\KERNEL.EFI...\r\n");

    status = boot_services->handle_protocol(image_handle, &loaded_image_protocol_guid, (void **)&loaded_image);
    if (EFI_ERROR(status)) {
        halt_with_error(console, "cannot inspect boot image");
    }

    status = boot_services->handle_protocol(
        loaded_image->device_handle,
        &simple_file_system_protocol_guid,
        (void **)&file_system
    );
    if (EFI_ERROR(status)) {
        halt_with_error(console, "boot volume has no supported filesystem");
    }

    status = file_system->open_volume(file_system, &root);
    if (EFI_ERROR(status)) {
        halt_with_error(console, "cannot open boot volume");
    }

    status = root->open(root, &kernel_file, kernel_path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        halt_with_error(console, "cannot find kernel image");
    }

    status = root->open(root, &trampoline_file, trampoline_path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        halt_with_error(console, "cannot find AP trampoline");
    }

    status = boot_services->allocate_pages(
        EFI_ALLOCATE_ADDRESS,
        EFI_LOADER_DATA,
        1,
        &trampoline_address
    );
    if (EFI_ERROR(status) || trampoline_address != 0x8000) {
        halt_with_error(console, "cannot allocate AP trampoline page");
    }

    status = trampoline_file->read(
        trampoline_file,
        &trampoline_size,
        (void *)(UINTN)trampoline_address
    );
    trampoline_file->close(trampoline_file);
    if (EFI_ERROR(status) || trampoline_size != 4096) {
        halt_with_error(console, "cannot read AP trampoline");
    }

    status = boot_services->allocate_pool(EFI_LOADER_DATA, kernel_size, &kernel_buffer);
    if (EFI_ERROR(status)) {
        halt_with_error(console, "cannot allocate kernel staging memory");
    }

    status = kernel_file->read(kernel_file, &kernel_size, kernel_buffer);
    kernel_file->close(kernel_file);
    if (EFI_ERROR(status)) {
        halt_with_error(console, "cannot read kernel image");
    }

    (void)read_file_to_pool(boot_services, root, disk_text_path, 512, &disk_text_buffer, &disk_text_size);
    (void)read_file_to_pool(boot_services, root, readme_path, 512, &readme_buffer, &readme_size);
    (void)read_file_to_pool(boot_services, root, user_program_path, 65536, &user_program_buffer, &user_program_size);
    root->close(root);

    status = boot_services->load_image(0, image_handle, 0, kernel_buffer, kernel_size, &kernel_handle);
    if (EFI_ERROR(status)) {
        halt_with_error(console, "firmware rejected kernel image");
    }

    status = boot_services->locate_protocol(&graphics_output_protocol_guid, 0, (void **)&graphics);
    if (EFI_ERROR(status) || graphics->mode == 0 || graphics->mode->info == 0) {
        halt_with_error(console, "cannot acquire graphics framebuffer");
    }

    status = boot_services->allocate_pool(EFI_LOADER_DATA, sizeof(ASAS_BOOT_INFO), (void **)&boot_info);
    if (EFI_ERROR(status)) {
        halt_with_error(console, "cannot allocate BootInfo");
    }

    boot_info->magic = ASAS_BOOT_INFO_MAGIC;
    boot_info->version = ASAS_BOOT_INFO_VERSION;
    boot_info->framebuffer_base = graphics->mode->framebuffer_base;
    boot_info->framebuffer_size = graphics->mode->framebuffer_size;
    boot_info->framebuffer_width = graphics->mode->info->horizontal_resolution;
    boot_info->framebuffer_height = graphics->mode->info->vertical_resolution;
    boot_info->framebuffer_stride = graphics->mode->info->pixels_per_scan_line;
    boot_info->framebuffer_pixel_format = graphics->mode->info->pixel_format;
    boot_info->acpi_rsdp = 0;
    boot_info->ap_trampoline = trampoline_address;
    boot_info->boot_disk_text_base = (UINT64)(UINTN)disk_text_buffer;
    boot_info->boot_disk_text_size = disk_text_buffer != 0 ? disk_text_size : 0;
    boot_info->boot_readme_base = (UINT64)(UINTN)readme_buffer;
    boot_info->boot_readme_size = readme_buffer != 0 ? readme_size : 0;
    boot_info->boot_user_program_base = (UINT64)(UINTN)user_program_buffer;
    boot_info->boot_user_program_size = user_program_buffer != 0 ? user_program_size : 0;

    for (table_index = 0; table_index < system_table->number_of_table_entries; table_index++) {
        EFI_CONFIGURATION_TABLE *table = &system_table->configuration_table[table_index];

        if (guid_equals(&table->vendor_guid, &acpi_20_table_guid)) {
            boot_info->acpi_rsdp = (UINT64)(UINTN)table->vendor_table;
            break;
        }
    }

    status = boot_services->handle_protocol(
        kernel_handle,
        &loaded_image_protocol_guid,
        (void **)&kernel_image
    );
    if (EFI_ERROR(status)) {
        halt_with_error(console, "cannot configure kernel BootInfo");
    }

    kernel_image->load_options = boot_info;
    kernel_image->load_options_size = sizeof(ASAS_BOOT_INFO);

    print_ascii(console, "Kernel loaded. Transferring control...\r\n");
    {
        static char fb_msg[64];
        UINT32 w = boot_info->framebuffer_width;
        UINT32 h = boot_info->framebuffer_height;
        UINT32 i = 0;
        const char *label = "FB: ";
        while (label[i]) fb_msg[i] = label[i], i++;
        /* width */
        if (w == 0) { fb_msg[i++] = '0'; } else {
            char tmp[8]; UINT32 j=0, ww=w;
            while(ww){tmp[j++]='0'+ww%10;ww/=10;}
            while(j--) fb_msg[i++]=tmp[j+1]; /* already decremented */
        }
        fb_msg[i++]='x';
        if (h == 0) { fb_msg[i++] = '0'; } else {
            char tmp[8]; UINT32 j=0, hh=h;
            while(hh){tmp[j++]='0'+hh%10;hh/=10;}
            while(j--) fb_msg[i++]=tmp[j+1];
        }
        fb_msg[i++]=' '; fb_msg[i++]='f'; fb_msg[i++]='m'; fb_msg[i++]='t';
        fb_msg[i++]='='; fb_msg[i++]=(char)('0'+boot_info->framebuffer_pixel_format);
        fb_msg[i++]='\r'; fb_msg[i++]='\n'; fb_msg[i]='\0';
        print_ascii(console, fb_msg);
    }
    status = boot_services->start_image(kernel_handle, 0, 0);
    if (EFI_ERROR(status)) {
        halt_with_error(console, "kernel returned with an error");
    }

    for (;;) {
        __halt();
    }
}
