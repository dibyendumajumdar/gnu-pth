/*
       Copyright Edward Nevill + Oliver Kowalke 2015
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

/*  Vendored from Boost.Context (boostorg/context, src/asm) for GNU Pth's
    "bctx" machine-context method. Boost Software License 1.0 (see
    fcontext/LICENSE_BOOST_1_0.txt). Only jump_fcontext/make_fcontext are used. */

.file "jump_arm64_aapcs_elf_gas.S"
.text
#if defined(__ARM_FEATURE_BTI_DEFAULT) && (__ARM_FEATURE_BTI_DEFAULT == 1)
/* Mark this object as requiring BTI, enabling the linker to set BTI on the output */
    .pushsection .note.gnu.property, "a", %note
    .p2align 3
    .long 4                      /* namesz */
    .long 16                     /* descsz (16 bytes) */
    .long 5                      /* type = NT_GNU_PROPERTY_TYPE_0 */
    .asciz "GNU"
    .p2align 3
    .long 0xc0000000             /* GNU_PROPERTY_AARCH64_FEATURE_1_AND */
    .long 4                      /* datasz = 4 */
    .long 1                      /* GNU_PROPERTY_AARCH64_FEATURE_1_BTI */
    .long 0                      /* pad to 8-byte alignment */
    .popsection
#endif
.align  2
.global jump_fcontext
.hidden jump_fcontext
.type   jump_fcontext, %function
jump_fcontext:
#if defined(__ARM_FEATURE_BTI_DEFAULT) && (__ARM_FEATURE_BTI_DEFAULT == 1)
    hint    #34                    /* bti c: valid indirect-entry target */
#endif
    # prepare stack for GP + FPU
    sub  sp, sp, #0xb0

    # save d8 - d15
    stp  d8,  d9,  [sp, #0x00]
    stp  d10, d11, [sp, #0x10]
    stp  d12, d13, [sp, #0x20]
    stp  d14, d15, [sp, #0x30]

    # save x19-x30
    stp  x19, x20, [sp, #0x40]
    stp  x21, x22, [sp, #0x50]
    stp  x23, x24, [sp, #0x60]
    stp  x25, x26, [sp, #0x70]
    stp  x27, x28, [sp, #0x80]
    stp  x29, x30, [sp, #0x90]

    # save LR as PC
    str  x30, [sp, #0xa0]

    # store RSP (pointing to context-data) in X0
    mov  x4, sp

    # restore RSP (pointing to context-data) from X1
    mov  sp, x0

    # load d8 - d15
    ldp  d8,  d9,  [sp, #0x00]
    ldp  d10, d11, [sp, #0x10]
    ldp  d12, d13, [sp, #0x20]
    ldp  d14, d15, [sp, #0x30]

    # load x19-x30
    ldp  x19, x20, [sp, #0x40]
    ldp  x21, x22, [sp, #0x50]
    ldp  x23, x24, [sp, #0x60]
    ldp  x25, x26, [sp, #0x70]
    ldp  x27, x28, [sp, #0x80]
    ldp  x29, x30, [sp, #0x90]

    # return transfer_t from jump
    # pass transfer_t as first arg in context function
    # X0 == FCTX, X1 == DATA
    mov x0, x4

    # load pc
    ldr  x4, [sp, #0xa0]

    # restore stack from GP + FPU
    add  sp, sp, #0xb0

    ret x4
.size   jump_fcontext,.-jump_fcontext
# Mark that we don't need executable stack.
.section .note.GNU-stack,"",%progbits
