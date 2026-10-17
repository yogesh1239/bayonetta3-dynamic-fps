.macro CODE_BEGIN name
	.section .text.\name, "ax", %progbits
	.global \name
	.type \name, %function
	.align 2
	.cfi_startproc
\name:
.endm

.macro CODE_END
	.cfi_endproc
.endm

/* Size of stack to reserve for the context. Adjust this along with CtxStackSize in inline_impl.cpp */
.set CTX_STACK_BASE_SIZE, 0x100
.set CTX_STACK_FLOAT_SIZE, 0x200

/* For these macros, LR is deliberately not backed up as that's handled by the entry's entrypoint. */
.macro armBackupRegisters
    sub sp, sp, CTX_STACK_BASE_SIZE
    armStoreGpr
.endm

.macro armStoreGpr
    stp x0, x1, [sp, #0x00]
    stp x2, x3, [sp, #0x10]
    stp x4, x5, [sp, #0x20]
    stp x6, x7, [sp, #0x30]
    stp x8, x9, [sp, #0x40]
    stp x10, x11, [sp, #0x50]
    stp x12, x13, [sp, #0x60]
    stp x14, x15, [sp, #0x70]
    stp x16, x17, [sp, #0x80]
    stp x18, x19, [sp, #0x90]
    stp x20, x21, [sp, #0xA0]
    stp x22, x23, [sp, #0xB0]
    stp x24, x25, [sp, #0xC0]
    stp x26, x27, [sp, #0xD0]
    stp x28, x29, [sp, #0xE0]
.endm

.macro armStoreFloat off
    stp q0, q1, [sp, #(\off+0x00)]
    stp q2, q3, [sp, #(\off+0x20)]
    stp q4, q5, [sp, #(\off+0x40)]
    stp q6, q7, [sp, #(\off+0x60)]
    stp q8, q9, [sp, #(\off+0x80)]
    stp q10, q11, [sp, #(\off+0xA0)]
    stp q12, q13, [sp, #(\off+0xC0)]
    stp q14, q15, [sp, #(\off+0xE0)]
    stp q16, q17, [sp, #(\off+0x100)]
    stp q18, q19, [sp, #(\off+0x120)]
    stp q20, q21, [sp, #(\off+0x140)]
    stp q22, q23, [sp, #(\off+0x160)]
    stp q24, q25, [sp, #(\off+0x180)]
    stp q26, q27, [sp, #(\off+0x1A0)]
    stp q28, q29, [sp, #(\off+0x1C0)]
    stp q30, q31, [sp, #(\off+0x1E0)]
.endm

.macro armRecoverRegisters
    armLoadGpr
    add sp, sp, CTX_STACK_BASE_SIZE
.endm

.macro armLoadGpr
    ldp x0, x1, [sp, #0x00]
    ldp x2, x3, [sp, #0x10]
    ldp x4, x5, [sp, #0x20]
    ldp x6, x7, [sp, #0x30]
    ldp x8, x9, [sp, #0x40]
    ldp x10, x11, [sp, #0x50]
    ldp x12, x13, [sp, #0x60]
    ldp x14, x15, [sp, #0x70]
    ldp x16, x17, [sp, #0x80]
    ldp x18, x19, [sp, #0x90]
    ldp x20, x21, [sp, #0xA0]
    ldp x22, x23, [sp, #0xB0]
    ldp x24, x25, [sp, #0xC0]
    ldp x26, x27, [sp, #0xD0]
    ldp x28, x29, [sp, #0xE0]
.endm

.macro armLoadFloat off
    ldp q0, q1, [sp, #(\off+0x00)]
    ldp q2, q3, [sp, #(\off+0x20)]
    ldp q4, q5, [sp, #(\off+0x40)]
    ldp q6, q7, [sp, #(\off+0x60)]
    ldp q8, q9, [sp, #(\off+0x80)]
    ldp q10, q11, [sp, #(\off+0xA0)]
    ldp q12, q13, [sp, #(\off+0xC0)]
    ldp q14, q15, [sp, #(\off+0xE0)]
    ldp q16, q17, [sp, #(\off+0x100)]
    ldp q18, q19, [sp, #(\off+0x120)]
    ldp q20, q21, [sp, #(\off+0x140)]
    ldp q22, q23, [sp, #(\off+0x160)]
    ldp q24, q25, [sp, #(\off+0x180)]
    ldp q26, q27, [sp, #(\off+0x1A0)]
    ldp q28, q29, [sp, #(\off+0x1C0)]
    ldp q30, q31, [sp, #(\off+0x1E0)]
.endm

/* IMPL-9 item 2: NZCV is also stored in the context (slot sp+0xF8, ctx->NZCV, bits 31..28 = N Z C V) and restored FROM it, so a callback can change the flags.
   NZCV: the callback is ordinary C++ and clobbers the flags. x21 is callee-saved (the callback keeps it) and the context already holds its
   original value, so it carries NZCV across the call, the same way x19 and x20 carry the entry pointer and the callback. */
CODE_BEGIN exl_inline_hook_impl
    armBackupRegisters
    mrs x21, nzcv
    str x21, [sp, #0xF8]

    /* LR contains a pointer to the called entry here. */
    mov x19, lr

    /* Load inline context for the first argument of the callback. */
    mov x0, sp
    /* Load then call callback. */
    ldr x20, [x19, #0x8]
    blr x20

    /* Keep a hold of entry pointer before restoring all the registers. */
    mov lr, x19

    ldr x21, [sp, #0xF8]
    msr nzcv, x21
    armRecoverRegisters

    /* Return to entry. */
    ret
CODE_END

/* Float variant. Frame = 0x300 bytes: GPRs at sp+0x00 (InlineCtx), floats at sp+0x100 (InlineFloatCtx::m_Fr).
   The entry stashed the original LR at (old sp - 0x10) = sp+0x2F0 here, which is where q31 is saved. So: copy that LR into the
   ctx slot sp+0xF0 before the float save, and put it back at sp+0x2F0 after the float restore (the entry reloads LR from there). */
CODE_BEGIN exl_inline_float_hook_impl
    sub sp, sp, #(CTX_STACK_BASE_SIZE + CTX_STACK_FLOAT_SIZE)
    armStoreGpr
    ldr x0, [sp, #(CTX_STACK_BASE_SIZE + CTX_STACK_FLOAT_SIZE - 0x10)]
    str x0, [sp, #0xF0]
    armStoreFloat CTX_STACK_BASE_SIZE
    mrs x21, nzcv
    str x21, [sp, #0xF8]

    /* LR contains a pointer to the called entry here. */
    mov x19, lr

    /* Load inline context for the first argument of the callback. */
    mov x0, sp
    /* Load then call callback. */
    ldr x20, [x19, #0x8]
    blr x20

    /* Keep a hold of entry pointer before restoring all the registers. */
    mov lr, x19

    ldr x21, [sp, #0xF8]
    msr nzcv, x21
    armLoadFloat CTX_STACK_BASE_SIZE
    ldr x0, [sp, #0xF0]
    str x0, [sp, #(CTX_STACK_BASE_SIZE + CTX_STACK_FLOAT_SIZE - 0x10)]
    armLoadGpr
    add sp, sp, #(CTX_STACK_BASE_SIZE + CTX_STACK_FLOAT_SIZE)

    /* Return to entry. */
    ret
CODE_END
