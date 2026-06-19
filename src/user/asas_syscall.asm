bits 64
default rel

section .text

global asas_system_call

asas_system_call:
    mov rax, rcx
    mov rbx, rdx
    mov rcx, r8
    mov rdx, r9
    int 0x80
    ret
