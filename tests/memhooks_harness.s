@ memhooks_harness.s - runs a stand-in from plugin/Sources/mem_hooks.s the way a game would reach
@ it through a CTRPF hook (game registers, lr and stack as they were, the HookContext in TLS slot
@ 15), and records every register when it comes back. For tests/test_memhooks_arm (qemu-arm).
@
@ Block (see test_memhooks_arm.cpp): +0 stand-in, +4 inputs r0-r3, +20 outputs r0-r12, +72 lr,
@ +76 cpsr, +80 sp, +84 sp before, +88 fake HookContext (32 bytes), +120 fake TLS (0x40),
@ +200 misalign the game's stack by 4.

    .arm
    .text
    .align 2

    .global RunStub
RunStub:
    push    {r4-r11, lr}
    ldr     r1, =g_block
    str     r0, [r1]
    ldr     r1, =g_savedSp
    str     sp, [r1]
    mrc     p15, 0, r1, c13, c0, 3
    ldr     r2, =g_origTls
    str     r1, [r2]
    add     r0, r0, #120              @ this thread's TLS -> the block's fake TLS
    ldr     r7, =0x0f0005             @ __ARM_NR_set_tls
    svc     #0
    ldr     r0, =g_block
    ldr     r0, [r0]
    ldr     r1, =Back
    str     r1, [r0, #100]            @ HookContext.returnAddress
    add     r1, r0, #88
    str     r1, [r0, #180]            @ TLS slot 15 (+0x3C) = &HookContext
    ldr     r1, [r0, #200]
    cmp     r1, #0
    subne   sp, sp, #4                @ a game stack that isn't 8-byte aligned
    ldr     r12, [r0]
    push    {r12}                     @ where to "jump from the hook"
    add     r1, sp, #4
    str     r1, [r0, #84]             @ the game's sp at the svc
    ldr     r4, =0x44444444
    ldr     r5, =0x55555555
    ldr     r6, =0x66666666
    ldr     r7, =0x77777777
    ldr     r8, =0x88888888
    ldr     r9, =0x99999999
    ldr     r10, =0xAAAAAAAA
    ldr     r11, =0xBBBBBBBB
    ldr     r12, =0xCCCCCCCC
    ldr     lr, =0xEEEEEEEE
    ldr     r1, [r0, #8]
    ldr     r2, [r0, #12]
    ldr     r3, [r0, #16]
    ldr     r0, [r0, #4]
    msr     cpsr_f, #0x60000000       @ Z and C set, N and V clear
    pop     {pc}

Back:
    push    {r0-r12, lr}
    mrs     r4, cpsr
    ldr     r5, =g_block
    ldr     r5, [r5]
    str     r4, [r5, #76]
    add     r6, sp, #56
    str     r6, [r5, #80]
    add     r7, r5, #20
    ldmia   sp!, {r0-r3}
    stmia   r7!, {r0-r3}
    ldmia   sp!, {r0-r3}
    stmia   r7!, {r0-r3}
    ldmia   sp!, {r0-r3}
    stmia   r7!, {r0-r3}
    ldmia   sp!, {r0-r1}
    stmia   r7!, {r0-r1}
    ldr     r0, =g_origTls
    ldr     r0, [r0]
    ldr     r7, =0x0f0005
    svc     #0
    ldr     r1, =g_savedSp
    ldr     sp, [r1]
    pop     {r4-r11, pc}

@ The system call, as the stand-ins see it (mem_hooks.s built with MEMHOOK_TEST): r12 = its
@ number. Like the kernel it keeps r3-r11 and the flags. 0x2A/0x2B: r0 = 0, r1:r2 = g_fakeValue.
@ 0x39/0x3A: r0 = 0, values[i] = g_fakeValue for the commit entries, 7 for others.
    .global KotobaTestSvc
KotobaTestSvc:
    push    {r4-r8}
    mrs     r8, cpsr
    ldr     r4, =g_lastSvc
    str     r12, [r4]
    ldr     r4, =g_fakeValue
    ldrd    r4, r5, [r4]
    cmp     r12, #0x39
    cmpne   r12, #0x3A
    beq     1f
    mov     r0, #0
    mov     r1, r4
    mov     r2, r5
    b       9f
1:  mov     r6, #0                    @ i
2:  cmp     r6, r3
    bge     8f
    ldr     r7, [r2, r6, lsl #2]
    cmp     r7, #1
    addeq   r7, r0, r6, lsl #3
    streqd  r4, r5, [r7]
    movne   r7, #7
    addne   r12, r0, r6, lsl #3
    strne   r7, [r12]
    movne   r7, #0
    strne   r7, [r12, #4]
    add     r6, r6, #1
    b       2b
8:  mov     r0, #0
9:  msr     cpsr_f, r8
    pop     {r4-r8}
    bx      lr

    .ltorg

    .data
    .align 3
    .global g_fakeValue, g_lastSvc
g_fakeValue:  .quad 0
g_lastSvc:    .word 0
g_block:      .word 0
g_savedSp:    .word 0
g_origTls:    .word 0
