/*
**  pth_fcontext.h: C declarations for the Boost.Context "fcontext" API used by
**  GNU Pth's "bctx" machine-context method (PTH_MCTX_MTH(bctx)).
**
**  The implementations live in the vendored assembly under fcontext/ (Boost
**  Software License 1.0). Only make_fcontext() and jump_fcontext() are used;
**  the symbols carry hidden ELF visibility, so they do not leak from a shared
**  libpth and do not clash with an application that also links Boost.Context.
*/
#ifndef _PTH_FCONTEXT_H_
#define _PTH_FCONTEXT_H_

#include <stddef.h>   /* size_t */

/* an opaque handle to a saved execution context (a stack pointer, really) */
typedef void *pth_fcontext_t;

/* result of a context switch: the continuation of the context that switched
   back to us (fctx) plus the datum it passed (data). ABI: two pointer-sized
   words, matching Boost.Context's boost::context::detail::transfer_t. */
typedef struct pth_transfer_st {
    pth_fcontext_t fctx;
    void          *data;
} pth_transfer_t;

/* create a context on the stack whose top (highest address) is `sp' and whose
   size is `size' bytes; it will begin executing `fn' on first jump. */
extern pth_fcontext_t make_fcontext(void *sp, size_t size,
                                    void (*fn)(pth_transfer_t));

/* switch to context `to', passing `vp'; returns the continuation of whoever
   switches back to us together with their datum. */
extern pth_transfer_t jump_fcontext(pth_fcontext_t const to, void *vp);

#endif /* _PTH_FCONTEXT_H_ */
