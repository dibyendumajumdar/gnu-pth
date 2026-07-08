/*
            Copyright Edward Nevill + Oliver Kowalke 2015
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

/*  Vendored from Boost.Context (boostorg/context, src/asm) for GNU Pth's
    "bctx" machine-context method. Boost Software License 1.0 (see
    fcontext/LICENSE_BOOST_1_0.txt). Only jump_fcontext/make_fcontext are used. */

.text
.private_extern _make_fcontext
.globl _make_fcontext
.balign 16

_make_fcontext:
    ; shift address in x0 (allocated stack) to lower 16 byte boundary
    and x0, x0, ~0xF

    ; reserve space for context-data on context-stack
    sub  x0, x0, #0xb0

    ; third arg of make_fcontext() == address of context-function
    ; store address as a PC to jump in
    str  x2, [x0, #0xa0]

    adr  x1, finish

    ; save address of finish as return-address for context-function
    ; will be entered after context-function returns (LR register)
    str  x1, [x0, #0x98]

    ret  lr ; return pointer to context-data (x0)

finish:
    ; exit code is zero
    mov  x0, #0
    ; exit application
    bl  __exit
