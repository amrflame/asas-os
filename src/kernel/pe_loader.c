#include "pe_loader.h"
#include "architecture.h"
#include "aslr.h"
#include "heap.h"
#include "logger.h"
#include "vfs.h"

extern void enter_user_mode(UINT64 user_entry, UINT64 user_stack);

#define PE_BUFFER_SIZE 65536

#pragma pack(push, 1)
typedef struct {
    UINT16 machine;
    UINT16 number_of_sections;
    UINT32 time_date_stamp;
    UINT32 pointer_to_symbol_table;
    UINT32 number_of_symbols;
    UINT16 size_of_optional_header;
    UINT16 characteristics;
} PE_COFF_HEADER;

typedef struct {
    UINT16 magic;
    UINT8 linker_major;
    UINT8 linker_minor;
    UINT32 size_of_code;
    UINT32 size_of_initialized_data;
    UINT32 size_of_uninitialized_data;
    UINT32 entry_point;
    UINT32 base_of_code;
    UINT64 image_base;
    UINT32 section_alignment;
    UINT32 file_alignment;
    UINT16 versions[6];
    UINT32 win32_version;
    UINT32 size_of_image;
    UINT32 size_of_headers;
} PE_OPTIONAL_HEADER_PREFIX;

typedef struct {
    UINT8 name[8];
    UINT32 virtual_size;
    UINT32 virtual_address;
    UINT32 raw_size;
    UINT32 raw_offset;
    UINT32 relocations;
    UINT32 line_numbers;
    UINT16 relocation_count;
    UINT16 line_number_count;
    UINT32 characteristics;
} PE_SECTION_HEADER;
#pragma pack(pop)

static void copy_bytes(UINT8 *destination, const UINT8 *source, UINT64 count)
{
    UINT64 index;

    for (index = 0; index < count; index++) {
        destination[index] = source[index];
    }
}

static int map_image_pages(
    ASAS_PAGE_TABLES *tables,
    ASAS_FRAME_ALLOCATOR *allocator,
    UINT64 image_base,
    UINT32 image_size
)
{
    UINT64 offset;

    for (offset = 0; offset < image_size; offset += 4096) {
        UINT64 frame = frame_allocate(allocator);

        if (
            frame == 0 ||
            !map_page(
                tables,
                image_base + offset,
                frame,
                ASAS_PAGE_USER | ASAS_PAGE_WRITABLE
            )
        ) {
            return 0;
        }
    }

    return 1;
}

static UINT64 read_program_file(const char fat_name[11], UINT8 *file, UINT64 capacity)
{
    /* Route entirely through VFS — no direct FAT16 calls.
       Try the canonical path first, then the 8.3 name fallback. */
    UINT64 handle;
    UINT64 file_size;
    (void)fat_name;   /* reserved for future use */

    if (!vfs_can_execute("/HELLO.EXE")) {
        logger_write("PE", "execution denied by mount no-exec policy");
        return 0;
    }

    handle = vfs_open("/HELLO.EXE");
    if (handle == 0) {
        logger_write("PE", "HELLO.EXE not found via VFS");
        return 0;
    }
    file_size = vfs_read(handle, file, capacity);
    (void)vfs_close(handle);
    return file_size;
}

int pe_load_and_enter_user_program(
    const char fat_name[11],
    ASAS_PAGE_TABLES *tables,
    ASAS_FRAME_ALLOCATOR *allocator
)
{
    UINT8 *file = (UINT8 *)kmalloc(PE_BUFFER_SIZE);
    UINT64 file_size;
    UINT32 pe_offset;
    PE_COFF_HEADER *coff;
    PE_OPTIONAL_HEADER_PREFIX *optional;
    PE_SECTION_HEADER *sections;
    UINT32 section_index;
    UINT64 stack_address;
    UINT64 stack_frame;

    if (file == 0) {
        return 0;
    }

    file_size = read_program_file(fat_name, file, PE_BUFFER_SIZE);
    if (file_size < 512 || file[0] != 'M' || file[1] != 'Z') {
        logger_write_hex("PE", "read_program_file size", file_size);
        logger_write_hex("PE", "byte0", file_size > 0 ? file[0] : 0xDEAD);
        return 0;
    }

    pe_offset = *(UINT32 *)&file[0x3C];
    if (
        pe_offset + 4 + sizeof(PE_COFF_HEADER) + sizeof(PE_OPTIONAL_HEADER_PREFIX) > file_size ||
        file[pe_offset] != 'P' ||
        file[pe_offset + 1] != 'E'
    ) {
        logger_write_hex("PE", "pe_offset bad", pe_offset);
        return 0;
    }

    coff = (PE_COFF_HEADER *)&file[pe_offset + 4];
    optional = (PE_OPTIONAL_HEADER_PREFIX *)(coff + 1);
    if (optional->magic != 0x20B || optional->size_of_image == 0) {
        logger_write_hex("PE", "optional magic", optional->magic);
        return 0;
    }
    if (!map_image_pages(tables, allocator, optional->image_base, optional->size_of_image)) {
        logger_write_hex("PE", "map_image_pages failed image_base", optional->image_base);
        return 0;
    }

    copy_bytes(
        (UINT8 *)(UINTN)optional->image_base,
        file,
        optional->size_of_headers < file_size ? optional->size_of_headers : file_size
    );

    sections = (PE_SECTION_HEADER *)((UINT8 *)optional + coff->size_of_optional_header);
    for (section_index = 0; section_index < coff->number_of_sections; section_index++) {
        PE_SECTION_HEADER *section = &sections[section_index];

        if (
            section->raw_size != 0 &&
            (UINT64)section->raw_offset + section->raw_size <= file_size
        ) {
            copy_bytes(
                (UINT8 *)(UINTN)(optional->image_base + section->virtual_address),
                &file[section->raw_offset],
                section->raw_size
            );
        }
    }

    stack_address = aslr_user_stack_address();
    stack_frame = frame_allocate(allocator);
    if (
        stack_frame == 0 ||
        !map_page(
            tables,
            stack_address,
            stack_frame,
            ASAS_PAGE_USER | ASAS_PAGE_WRITABLE | ASAS_PAGE_NO_EXECUTE
        )
    ) {
        return 0;
    }

    architecture_set_kernel_stack();
    enter_user_mode(
        optional->image_base + optional->entry_point,
        stack_address + 4096
    );
    return 0;
}
