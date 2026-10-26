@ mem_hooks.s - stand-ins for the four system calls a game uses to ask about its memory.
@
@ At boot the plugin patches each such `svc` instruction in the game's code with a CTRPF hook
@ (no flags: CTRPF jumps here with the game's registers, lr and stack as they were, and keeps the
@ hook's HookContext in this thread's TLS slot 15). Each stand-in makes the real call, lets
@ KotobaMemHookAdjust (main.cpp -> core/mem_share.cpp) take the plugin's share off the answer,
@ and goes back to the instruction after the svc with every other register as the svc leaves it.
@
@ Frame (8-byte aligned for the C call): [0..2] the call's outputs r0-r2, [4..7] its inputs r0-r3.
@ The address to go back to comes from this thread's own HookContext, so game threads calling at
@ the same time can't mix theirs up.

    .arm
    .section .text.kotoba_mem_hooks, "ax", %progbits
    .align 2

.macro MEMHOOK name, num
    .global \name
    .type \name, %function
\name:
    sub     sp, sp, #4                  @ slot: where to go back to
    push    {r3-r12, lr}                @ what the svc keeps, and the game's lr (44 bytes)
    mrc     p15, 0, r12, c13, c0, 3     @ this thread's TLS
    ldr     r12, [r12, #0x3C]           @ slot 15: CTRPF's HookContext for this hook
    ldr     r12, [r12, #12]             @ its returnAddress: the instruction after the svc
    str     r12, [sp, #44]
    mov     r4, sp
    bic     sp, sp, #7
    sub     sp, sp, #32
    add     r12, sp, #16
    stmia   r12, {r0-r3}                @ the call's inputs
.ifdef MEMHOOK_TEST
    mov     r12, #\num                  @ tests/test_memhooks_arm: a stand-in for the system call
    bl      KotobaTestSvc
.else
    svc     \num
.endif
    mrs     r5, cpsr                    @ the flags as the svc leaves them
    stmia   sp, {r0-r2}                 @ the call's outputs
    mov     r0, #\num
    mov     r1, sp
    bl      KotobaMemHookAdjust
    ldmia   sp, {r0-r2}
    msr     cpsr_f, r5
    mov     sp, r4
    pop     {r3-r12, lr}
    pop     {pc}
    .size \name, . - \name
.endm

    MEMHOOK KotobaHookGetSystemInfo, 0x2A
    MEMHOOK KotobaHookGetProcessInfo, 0x2B
    MEMHOOK KotobaHookLimitValues, 0x39
    MEMHOOK KotobaHookCurrentValues, 0x3A
