/*
            Copyright Edward Nevill + Oliver Kowalke 2015
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

/*  Vendored from Boost.Context (boostorg/context, src/asm) for GNU Pth's
    "bctx" machine-context method. Boost Software License 1.0 (see
    fcontext/LICENSE_BOOST_1_0.txt). Only jump_fcontext/make_fcontext are used. */

.file "make_arm64_aapcs_elf_gas.S"
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
.global make_fcontext
.hidden make_fcontext
.type   make_fcontext, %function
make_fcontext:
#if defined(__ARM_FEATURE_BTI_DEFAULT) && (__ARM_FEATURE_BTI_DEFAULT == 1)
    hint    #34    /* bti c */
#endif
    # shift address in x0 (allocated stack) to lower 16 byte boundary
    and x0, x0, ~0xF

    # reserve space for context-data on context-stack
    sub  x0, x0, #0xb0

    # third arg of make_fcontext() == address of context-function
    # store address as a PC to jump in
    str  x2, [x0, #0xa0]

    # save address of finish as return-address for context-function
    # will be entered after context-function returns (LR register)
    adr  x1, finish
    str  x1, [x0, #0x98]

    ret  x30 // return pointer to context-data (x0)

finish:
    # exit code is zero
    mov  x0, #0
    # exit application
    bl  _exit

.size   make_fcontext,.-make_fcontext
# Mark that we don't need executable stack.
.section .note.GNU-stack,"",%progbits
