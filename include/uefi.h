#ifndef ASAS_UEFI_H
#define ASAS_UEFI_H

typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64;
typedef UINT64 UINTN;
typedef UINT64 EFI_STATUS;
typedef void *EFI_HANDLE;
typedef UINT16 CHAR16;

#define EFI_SUCCESS 0
#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ULL
#define EFI_ERROR(status) (((status) & 0x8000000000000000ULL) != 0)
#define EFI_LOADER_DATA 2
#define EFI_ALLOCATE_ADDRESS 2
#define EFI_FILE_MODE_READ 0x0000000000000001ULL

typedef struct {
    UINT32 data1;
    UINT16 data2;
    UINT16 data3;
    UINT8 data4[8];
} EFI_GUID;

typedef struct {
    UINT64 signature;
    UINT32 revision;
    UINT32 header_size;
    UINT32 crc32;
    UINT32 reserved;
} EFI_TABLE_HEADER;

typedef struct EFI_SYSTEM_TABLE EFI_SYSTEM_TABLE;
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef struct {
    UINT32 type;
    UINT32 padding;
    UINT64 physical_start;
    UINT64 virtual_start;
    UINT64 number_of_pages;
    UINT64 attributes;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
    EFI_GUID vendor_guid;
    void *vendor_table;
} EFI_CONFIGURATION_TABLE;

typedef EFI_STATUS (*EFI_TEXT_STRING)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self,
    CHAR16 *string
);

typedef EFI_STATUS (*EFI_TEXT_CLEAR_SCREEN)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self);
typedef EFI_STATUS (*EFI_TEXT_SET_ATTRIBUTE)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, UINTN attribute);

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *reset;
    EFI_TEXT_STRING output_string;
    void *test_string;
    void *query_mode;
    void *set_mode;
    EFI_TEXT_SET_ATTRIBUTE set_attribute;
    EFI_TEXT_CLEAR_SCREEN clear_screen;
    void *set_cursor_position;
    void *enable_cursor;
    void *mode;
};

typedef EFI_STATUS (*EFI_ALLOCATE_POOL)(UINT32 pool_type, UINTN size, void **buffer);
typedef EFI_STATUS (*EFI_FREE_POOL)(void *buffer);
typedef EFI_STATUS (*EFI_ALLOCATE_PAGES)(UINT32 type, UINT32 memory_type, UINTN pages, UINT64 *memory);
typedef EFI_STATUS (*EFI_GET_MEMORY_MAP)(
    UINTN *memory_map_size,
    void *memory_map,
    UINTN *map_key,
    UINTN *descriptor_size,
    UINT32 *descriptor_version
);
typedef EFI_STATUS (*EFI_HANDLE_PROTOCOL)(EFI_HANDLE handle, EFI_GUID *protocol, void **interface);
typedef EFI_STATUS (*EFI_LOAD_IMAGE)(
    UINT8 boot_policy,
    EFI_HANDLE parent_image_handle,
    void *device_path,
    void *source_buffer,
    UINTN source_size,
    EFI_HANDLE *image_handle
);
typedef EFI_STATUS (*EFI_START_IMAGE)(EFI_HANDLE image_handle, UINTN *exit_data_size, CHAR16 **exit_data);
typedef EFI_STATUS (*EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE image_handle, UINTN map_key);
typedef EFI_STATUS (*EFI_LOCATE_PROTOCOL)(EFI_GUID *protocol, void *registration, void **interface);

typedef struct {
    EFI_TABLE_HEADER header;
    void *raise_tpl;
    void *restore_tpl;
    EFI_ALLOCATE_PAGES allocate_pages;
    void *free_pages;
    EFI_GET_MEMORY_MAP get_memory_map;
    EFI_ALLOCATE_POOL allocate_pool;
    EFI_FREE_POOL free_pool;
    void *create_event;
    void *set_timer;
    void *wait_for_event;
    void *signal_event;
    void *close_event;
    void *check_event;
    void *install_protocol_interface;
    void *reinstall_protocol_interface;
    void *uninstall_protocol_interface;
    EFI_HANDLE_PROTOCOL handle_protocol;
    void *reserved;
    void *register_protocol_notify;
    void *locate_handle;
    void *locate_device_path;
    void *install_configuration_table;
    EFI_LOAD_IMAGE load_image;
    EFI_START_IMAGE start_image;
    void *exit;
    void *unload_image;
    EFI_EXIT_BOOT_SERVICES exit_boot_services;
    void *get_next_monotonic_count;
    void *stall;
    void *set_watchdog_timer;
    void *connect_controller;
    void *disconnect_controller;
    void *open_protocol;
    void *close_protocol;
    void *open_protocol_information;
    void *protocols_per_handle;
    void *locate_handle_buffer;
    EFI_LOCATE_PROTOCOL locate_protocol;
    void *install_multiple_protocol_interfaces;
    void *uninstall_multiple_protocol_interfaces;
    void *calculate_crc32;
    void *copy_mem;
    void *set_mem;
    void *create_event_ex;
} EFI_BOOT_SERVICES;

typedef struct {
    UINT32 revision;
    EFI_HANDLE parent_handle;
    EFI_SYSTEM_TABLE *system_table;
    EFI_HANDLE device_handle;
    void *file_path;
    void *reserved;
    UINT32 load_options_size;
    void *load_options;
    void *image_base;
    UINT64 image_size;
    UINT32 image_code_type;
    UINT32 image_data_type;
    void *unload;
} EFI_LOADED_IMAGE_PROTOCOL;

typedef EFI_STATUS (*EFI_OPEN_VOLUME)(void *self, EFI_FILE_PROTOCOL **root);

typedef struct {
    UINT64 revision;
    EFI_OPEN_VOLUME open_volume;
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef EFI_STATUS (*EFI_FILE_OPEN)(
    EFI_FILE_PROTOCOL *self,
    EFI_FILE_PROTOCOL **new_handle,
    CHAR16 *file_name,
    UINT64 open_mode,
    UINT64 attributes
);
typedef EFI_STATUS (*EFI_FILE_CLOSE)(EFI_FILE_PROTOCOL *self);
typedef EFI_STATUS (*EFI_FILE_READ)(EFI_FILE_PROTOCOL *self, UINTN *buffer_size, void *buffer);

struct EFI_FILE_PROTOCOL {
    UINT64 revision;
    EFI_FILE_OPEN open;
    EFI_FILE_CLOSE close;
    void *delete_file;
    EFI_FILE_READ read;
    void *write;
    void *get_position;
    void *set_position;
    void *get_info;
    void *set_info;
    void *flush;
};

typedef struct {
    UINT32 red_mask;
    UINT32 green_mask;
    UINT32 blue_mask;
    UINT32 reserved_mask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32 version;
    UINT32 horizontal_resolution;
    UINT32 vertical_resolution;
    UINT32 pixel_format;
    EFI_PIXEL_BITMASK pixel_information;
    UINT32 pixels_per_scan_line;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 max_mode;
    UINT32 mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    UINTN size_of_info;
    UINT64 framebuffer_base;
    UINTN framebuffer_size;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    void *query_mode;
    void *set_mode;
    void *blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode;
};

struct EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER header;
    CHAR16 *firmware_vendor;
    UINT32 firmware_revision;
    EFI_HANDLE console_in_handle;
    void *con_in;
    EFI_HANDLE console_out_handle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con_out;
    EFI_HANDLE standard_error_handle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *std_err;
    void *runtime_services;
    EFI_BOOT_SERVICES *boot_services;
    UINTN number_of_table_entries;
    EFI_CONFIGURATION_TABLE *configuration_table;
};

#endif
