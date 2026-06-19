#include "architecture.h"
#include "logger.h"
#include "panic.h"

#pragma intrinsic(__readcr2)
UINT64 __readcr2(void);
#pragma intrinsic(__readcr4)
UINT64 __readcr4(void);
#pragma intrinsic(__writecr4)
void __writecr4(UINT64 value);
#pragma intrinsic(__cpuidex)
void __cpuidex(int cpuInfo[4], int function, int subLeaf);

#pragma pack(push, 1)
typedef struct {
    UINT16 limit;
    UINT64 base;
} DESCRIPTOR_TABLE_POINTER;

typedef struct {
    UINT16 offset_low;
    UINT16 selector;
    UINT8 ist;
    UINT8 attributes;
    UINT16 offset_middle;
    UINT32 offset_high;
    UINT32 reserved;
} IDT_ENTRY;

typedef struct {
    UINT32 reserved0;
    UINT64 rsp0;
    UINT64 rsp1;
    UINT64 rsp2;
    UINT64 reserved1;
    UINT64 ist1;
    UINT64 ist2;
    UINT64 ist3;
    UINT64 ist4;
    UINT64 ist5;
    UINT64 ist6;
    UINT64 ist7;
    UINT64 reserved2;
    UINT16 reserved3;
    UINT16 io_map_base;
} TSS;
#pragma pack(pop)

extern void gdt_load(const DESCRIPTOR_TABLE_POINTER *pointer);
extern void idt_load(const DESCRIPTOR_TABLE_POINTER *pointer);
extern void interrupt_default(void);
extern void interrupt_external_default(void);
extern void interrupt_spurious(void);
extern void interrupt_divide_error(void);
extern void interrupt_invalid_opcode(void);
extern void interrupt_general_protection(void);
extern void interrupt_double_fault(void);
extern void interrupt_page_fault(void);
extern void interrupt_apic_timer(void);
extern void interrupt_keyboard(void);
extern void interrupt_hyperv_vmbus(void);
extern void interrupts_enable(void);
extern void interrupt_syscall(void);
extern void tss_load(UINT16 selector);
extern UINT64 read_stack_pointer(void);

static UINT64 gdt[] = {
    0x0000000000000000ULL,
    0x00AF9A000000FFFFULL,
    0x00CF92000000FFFFULL,
    0x00CFF2000000FFFFULL,
    0x00AFFA000000FFFFULL,
    0,
    0
};

static IDT_ENTRY idt[256];
static TSS tss;
static UINT8 double_fault_stack[4096];

static void tss_install_descriptor(void)
{
    UINT64 base = (UINT64)(UINTN)&tss;
    UINT64 limit = sizeof(tss) - 1;

    gdt[5] =
        (limit & 0xFFFFULL) |
        ((base & 0xFFFFFFULL) << 16) |
        (0x89ULL << 40) |
        (((limit >> 16) & 0xFULL) << 48) |
        (((base >> 24) & 0xFFULL) << 56);
    gdt[6] = base >> 32;
}

static void idt_set_entry(UINT32 vector, void (*handler)(void))
{
    UINT64 address = (UINT64)(UINTN)handler;
    IDT_ENTRY *entry = &idt[vector];

    entry->offset_low = (UINT16)(address & 0xFFFF);
    entry->selector = 0x08;
    entry->ist = 0;
    entry->attributes = 0x8E;
    entry->offset_middle = (UINT16)((address >> 16) & 0xFFFF);
    entry->offset_high = (UINT32)(address >> 32);
    entry->reserved = 0;
}

void exception_panic(UINT64 vector, UINT64 error_code, UINT64 instruction_pointer)
{
    logger_write_hex("ERROR", "exception vector", vector);
    logger_write_hex("ERROR", "exception error code", error_code);
    logger_write_hex("ERROR", "exception instruction pointer", instruction_pointer);

    if (vector == 0) {
        panic("CPU divide error");
    }

    if (vector == 14) {
        logger_write_hex("ERROR", "page fault address", __readcr2());
        panic("CPU page fault");
    }

    if (vector == 6) {
        panic("CPU invalid opcode");
    }

    if (vector == 13) {
        panic("CPU general protection fault");
    }

    panic("unhandled CPU exception or interrupt");
}

static void architecture_enable_smep_smap(void)
{
    int cpuid_result[4];
    UINT64 cr4;

    __cpuidex(cpuid_result, 7, 0);
    cr4 = __readcr4();

    if (cpuid_result[1] & (1 << 7)) {
        cr4 |= (1ULL << 20);
        logger_write("INFO", "SMEP enabled");
    }
    if (cpuid_result[1] & (1 << 20)) {
        cr4 |= (1ULL << 21);
        logger_write("INFO", "SMAP enabled");
    }

    __writecr4(cr4);
}

void architecture_initialize(void)
{
    DESCRIPTOR_TABLE_POINTER gdt_pointer;
    DESCRIPTOR_TABLE_POINTER idt_pointer;
    UINT32 vector;

    gdt_pointer.limit = (UINT16)(sizeof(gdt) - 1);
    gdt_pointer.base = (UINT64)(UINTN)&gdt[0];
    tss.io_map_base = sizeof(tss);
    tss.ist1 = (UINT64)(UINTN)&double_fault_stack[sizeof(double_fault_stack)];
    tss_install_descriptor();
    gdt_load(&gdt_pointer);
    tss_load(0x28);
    logger_write("INFO", "kernel GDT installed");

    for (vector = 0; vector < 32; vector++) {
        idt_set_entry(vector, interrupt_default);
    }
    for (vector = 32; vector < 256; vector++) {
        idt_set_entry(vector, interrupt_external_default);
    }

    idt_set_entry(0, interrupt_divide_error);
    idt_set_entry(6, interrupt_invalid_opcode);
    idt_set_entry(8, interrupt_double_fault);
    idt[8].ist = 1;
    idt_set_entry(14, interrupt_page_fault);
    idt_set_entry(13, interrupt_general_protection);
    idt_set_entry(32, interrupt_apic_timer);
    idt_set_entry(33, interrupt_keyboard);
    idt_set_entry(64, interrupt_hyperv_vmbus);
    idt_set_entry(128, interrupt_syscall);
    idt[128].attributes = 0xEE;
    idt_set_entry(255, interrupt_spurious);

    idt_pointer.limit = (UINT16)(sizeof(idt) - 1);
    idt_pointer.base = (UINT64)(UINTN)&idt[0];
    idt_load(&idt_pointer);
    logger_write("INFO", "kernel IDT installed");

    architecture_enable_smep_smap();
}

void architecture_set_kernel_stack(void)
{
    tss.rsp0 = read_stack_pointer();
}
