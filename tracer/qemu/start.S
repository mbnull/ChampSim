.section .text
.global _start

_start:
    la sp, stack_top
    call main
    j tohost_exit


tohost_exit:
        la t0, tohost
        sw a0, 0(t0)
        1: j 1b

.section .tohost, "aw", @progbits
.globl tohost
.align 4
tohost: .dword 0
.globl fromhost
.align 4
fromhost: .dword 0

.section .bss
.space 4096
stack_top:
