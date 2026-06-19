bits 16
org 0x8000

start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    lgdt [gdt_pointer]

    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:protected_mode

bits 32
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov eax, [trampoline_cr3]
    mov cr3, eax
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    jmp 0x18:long_mode

bits 64
long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, [trampoline_stack]
    mov rax, [trampoline_entry]
    sub rsp, 32
    call rax

.halt:
    cli
    hlt
    jmp .halt

align 8
gdt:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
    dq 0x00AF9A000000FFFF
gdt_end:

gdt_pointer:
    dw gdt_end - gdt - 1
    dd gdt

times 0x100 - ($ - $$) db 0
trampoline_cr3:
    dq 0
trampoline_stack:
    dq 0
trampoline_entry:
    dq 0

times 4096 - ($ - $$) db 0
