bits 64
default rel

section .text

global gdt_load
global idt_load
global interrupt_default
global interrupt_external_default
global interrupt_spurious
global interrupt_divide_error
global interrupt_invalid_opcode
global interrupt_general_protection
global interrupt_double_fault
global interrupt_page_fault
global interrupt_apic_timer
global interrupt_keyboard
global interrupt_hyperv_vmbus
global interrupts_enable
global context_switch
global tss_load
global read_stack_pointer
global enter_user_mode
global interrupt_syscall
global memory_fence
global __chkstk

extern exception_panic
extern apic_timer_interrupt_handler
extern keyboard_interrupt_handler
extern hyperv_storage_interrupt_handler
extern apic_eoi
extern syscall_dispatch

gdt_load:
    lgdt [rcx]
    push qword 0x08
    lea rax, [rel .reload_code]
    push rax
    retfq

.reload_code:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    ret

idt_load:
    lidt [rcx]
    ret

interrupt_default:
    mov rcx, 255
    xor rdx, rdx
    mov r8, [rsp]
    jmp exception_entry

interrupt_external_default:
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    sub rsp, 32
    call apic_eoi
    add rsp, 32
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    iretq

interrupt_spurious:
    iretq

interrupt_divide_error:
    xor rcx, rcx
    xor rdx, rdx
    mov r8, [rsp]
    jmp exception_entry

interrupt_invalid_opcode:
    mov rcx, 6
    xor rdx, rdx
    mov r8, [rsp]
    jmp exception_entry

interrupt_general_protection:
    mov rcx, 13
    mov rdx, [rsp]
    mov r8, [rsp + 8]
    jmp exception_entry

; Double fault (vector 8) — always runs on IST1 emergency stack.
; The CPU pushes an error code of 0 before RIP.
interrupt_double_fault:
    mov rcx, 8
    xor rdx, rdx
    mov r8, [rsp + 8]
    jmp exception_entry

interrupt_page_fault:
    mov rcx, 14
    mov rdx, [rsp]
    mov r8, [rsp + 8]
    jmp exception_entry

interrupt_apic_timer:
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    sub rsp, 32
    call apic_timer_interrupt_handler
    add rsp, 32
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    iretq

interrupt_keyboard:
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    sub rsp, 32
    call keyboard_interrupt_handler
    add rsp, 32
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    iretq

interrupt_hyperv_vmbus:
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    sub rsp, 32
    call hyperv_storage_interrupt_handler
    add rsp, 32
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    iretq

global hyperv_do_hypercall
hyperv_do_hypercall:
    ; Windows x64 ABI input:
    ; rcx = hypercall page, rdx = control, r8 = input GPA, r9 = output GPA
    mov r10, rcx
    mov rcx, rdx
    mov rdx, r8
    mov r8, r9
    call r10
    ret

interrupts_enable:
    sti
    ret

context_switch:
    push rbx
    push rbp
    push rdi
    push rsi
    push r12
    push r13
    push r14
    push r15
    mov [rcx], rsp
    mov rsp, rdx
    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi
    pop rbp
    pop rbx
    ret

tss_load:
    mov ax, cx
    ltr ax
    ret

read_stack_pointer:
    mov rax, rsp
    ret

memory_fence:
    mfence
    ret

; __chkstk — stack probe for large stack frames (RAX = bytes to allocate).
; Called by MSVC when a function allocates more than 4096 bytes on the stack.
; Probes each 4096-byte page from the current stack top downward so the OS
; can extend the stack guard if needed.  In our kernel every thread has a
; fully committed stack, so the actual probing is a no-op; we just return.
__chkstk:
    ret

enter_user_mode:
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    push qword 0x1B
    push rdx
    pushfq
    pop rax
    or rax, 0x200
    push rax
    push qword 0x23
    push rcx
    iretq

interrupt_syscall:
    mov r9, rdx
    mov r8, rcx
    mov rdx, rbx
    mov rcx, rax
    sub rsp, 32
    call syscall_dispatch
    add rsp, 32
    iretq

exception_entry:
    and rsp, -16
    sub rsp, 32
    call exception_panic

.halt:
    cli
    hlt
    jmp .halt
