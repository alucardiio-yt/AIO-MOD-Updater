.section .text.aioNroTrampoline, "ax", %progbits
.align 2
.global aioNroTrampoline
.type aioNroTrampoline, %function
.cfi_startproc
aioNroTrampoline:
    adrp x8, __stack_top
    ldr  x8, [x8, #:lo12:__stack_top]
    mov  sp, x8
    blr  x2
    adrp x1, s_previous_result
    str  w0, [x1, #:lo12:s_previous_result]
    adrp x8, __stack_top
    ldr  x8, [x8, #:lo12:__stack_top]
    mov  sp, x8
    b    aioLoadTarget
.cfi_endproc

.section .text.__libnx_exception_entry, "ax", %progbits
.align 2
.global __libnx_exception_entry
.type __libnx_exception_entry, %function
.cfi_startproc
__libnx_exception_entry:
    adrp x7, s_mapped_addr
    ldr  x7, [x7, #:lo12:s_mapped_addr]
    cbz  x7, .Lno_target
    br   x7
.Lno_target:
    mov w0, #0xf801
    svc 0x28
.cfi_endproc
