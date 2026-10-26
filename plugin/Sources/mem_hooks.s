@ mem_hooks.s - stand-ins for the four system calls a game uses to ask about its memory.
@
@ At boot the plugin patches each such `svc` in the game's code to jump to a trampoline (built by
@ core/mem_share.cpp's buildTrampoline), which jumps here with every register, the flags and lr as
@ the game had them, except: sp is 8 lower, [sp] holds the game's r12 and [sp + 4] where to carry
@ on (the trampoline's copy of the instruction the patch moved). Each stand-in makes the real
@ call, lets KotobaMemHookAdjust (main.cpp -> core/mem_share.cpp) take the plugin's share off the
@ answer, and carries on with every other register as the svc leaves it. Nothing shared: game
@ threads can call at the same time.
@
@ Frame (8-byte aligned for the C call): [0..2] the call's outputs r0-r2, [4..7] its inputs r0-r3.

    .arm
    .section .text.kotoba_mem_hooks, "ax", %progbits
    .align 2

.macro MEMHOOK name, num
    .global \name
    .type \name, %function
\name:
    push    {r3-r11, lr}                @ what the svc keeps, and the game's lr (40 bytes)
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
    pop     {r3-r11, lr}
    pop     {r12}                       @ the game's r12 (from the trampoline)
    pop     {pc}                        @ carry on in the trampoline; sp is the game's again
    .size \name, . - \name
.endm

    MEMHOOK KotobaHookGetSystemInfo, 0x2A
    MEMHOOK KotobaHookGetProcessInfo, 0x2B
    MEMHOOK KotobaHookLimitValues, 0x39
    MEMHOOK KotobaHookCurrentValues, 0x3A
