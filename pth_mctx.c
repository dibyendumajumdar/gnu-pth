/*
**  GNU Pth - The GNU Portable Threads
**  Copyright (c) 1999-2006 Ralf S. Engelschall <rse@engelschall.com>
**
**  This file is part of GNU Pth, a non-preemptive thread scheduling
**  library which can be found at http://www.gnu.org/software/pth/.
**
**  This library is free software; you can redistribute it and/or
**  modify it under the terms of the GNU Lesser General Public
**  License as published by the Free Software Foundation; either
**  version 2.1 of the License, or (at your option) any later version.
**
**  This library is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
**  Lesser General Public License for more details.
**
**  You should have received a copy of the GNU Lesser General Public
**  License along with this library; if not, write to the Free Software
**  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
**  USA, or contact Ralf S. Engelschall <rse@engelschall.com>.
**
**  pth_mctx.c: Pth machine context handling
*/
                             /* ``If you can't do it in
                                  ANSI C, it isn't worth doing.'' 
                                                -- Unknown        */
#include "pth_p.h"

#if cpp

/*
 * machine context state structure
 *
 * In `jb' the CPU registers, the program counter, the stack
 * pointer and (usually) the signals mask is stored. When the
 * signal mask cannot be implicitly stored in `jb', it's
 * alternatively stored explicitly in `sigs'. The `error' stores
 * the value of `errno'.
 */

#if PTH_MCTX_MTH(mcsc)
#include <ucontext.h>
#endif
#if PTH_MCTX_MTH(bctx)
#include "pth_fcontext.h"
#endif

typedef struct pth_mctx_st pth_mctx_t;
struct pth_mctx_st {
#if PTH_MCTX_MTH(mcsc)
    ucontext_t uc;
    int restored;
#elif PTH_MCTX_MTH(bctx)
    pth_fcontext_t fc;      /* current continuation (Boost.Context)         */
    void (*func)(void);     /* entry function of a freshly-made context     */
    sigset_t sysmask;       /* real signal mask, saved/restored on switch   */
#else
#error "unknown mctx method"
#endif
    sigset_t sigs;
    int error;
};

/*
** ____ MACHINE STATE SWITCHING ______________________________________
*/

/*
 * save the current machine context
 */
#if PTH_MCTX_MTH(mcsc)
#define pth_mctx_save(mctx) \
        ( (mctx)->error = errno, \
          (mctx)->restored = 0, \
          getcontext(&(mctx)->uc), \
          (mctx)->restored )
#elif PTH_MCTX_MTH(bctx)
#define pth_mctx_save(mctx) (0) /* unused for bctx: switch is a single primitive */
#else
#error "unknown mctx method"
#endif

/*
 * restore the current machine context
 * (at the location of the old context)
 */
#if PTH_MCTX_MTH(mcsc)
#define pth_mctx_restore(mctx) \
        ( errno = (mctx)->error, \
          (mctx)->restored = 1, \
          (void)setcontext(&(mctx)->uc) )
#elif PTH_MCTX_MTH(bctx)
#define pth_mctx_restore(mctx) pth_mctx_restore_bctx(mctx)
#else
#error "unknown mctx method"
#endif

/*
 * restore the current machine context
 * (at the location of the new context)
 */
#define pth_mctx_restored(mctx) /*nop*/

/*
 * switch from one machine context to another
 */
#define SWITCH_DEBUG_LINE \
        "==== THREAD CONTEXT SWITCH ==========================================="
#ifdef PTH_DEBUG
#define  _pth_mctx_switch_debug pth_debug(NULL, 0, 1, SWITCH_DEBUG_LINE);
#else
#define  _pth_mctx_switch_debug /*NOP*/
#endif
#if PTH_MCTX_MTH(mcsc)
#define pth_mctx_switch(old,new) \
    _pth_mctx_switch_debug \
    swapcontext(&((old)->uc), &((new)->uc));
#elif PTH_MCTX_MTH(bctx)
#define pth_mctx_switch(old,new) \
    _pth_mctx_switch_debug \
    pth_mctx_switch_bctx((old),(new));
#else
#error "unknown mctx method"
#endif

#endif /* cpp */

/*
** ____ MACHINE STATE INITIALIZATION ________________________________
*/

#if PTH_MCTX_MTH(mcsc)

/*
 * VARIANT 1: THE STANDARDIZED SVR4/SUSv2 APPROACH
 *
 * This is the preferred variant, because it uses the standardized
 * SVR4/SUSv2 makecontext(2) and friends which is a facility intended
 * for user-space context switching. The thread creation therefore is
 * straight-foreward.
 */

intern int pth_mctx_set(
    pth_mctx_t *mctx, void (*func)(void), char *sk_addr_lo, char *sk_addr_hi)
{
    /* fetch current context */
    if (getcontext(&(mctx->uc)) != 0)
        return FALSE;

    /* remove parent link */
    mctx->uc.uc_link           = NULL;

    /* configure new stack */
    mctx->uc.uc_stack.ss_sp    = pth_skaddr(makecontext, sk_addr_lo, sk_addr_hi-sk_addr_lo);
    mctx->uc.uc_stack.ss_size  = pth_sksize(makecontext, sk_addr_lo, sk_addr_hi-sk_addr_lo);
    mctx->uc.uc_stack.ss_flags = 0;

    /* configure startup function (with no arguments) */
    makecontext(&(mctx->uc), func, 0+1);

    return TRUE;
}

#elif PTH_MCTX_MTH(bctx)

/*
 * VARIANT: BOOST.CONTEXT (fcontext) APPROACH
 *
 * A portable, assembly-based user-space context switch (vendored under
 * fcontext/, Boost Software License 1.0). Chosen where the SVR4/SUSv2
 * ucontext(3) facility is unavailable or deprecated (e.g. macOS/arm64).
 * Unlike ucontext, fcontext carries no signal mask, so the real signal mask
 * is saved/restored explicitly on every switch -- mirroring what
 * swapcontext(2) does implicitly for the mcsc method.
 *
 * fcontext bookkeeping: X->fc always holds the continuation to resume X, and
 * it is (re)written by whoever X switches to, at the moment they resume. The
 * caller passes its own mctx as the jump datum so the resumed side can record
 * it; a freshly made context finds its own mctx through pth_bctx_self, set
 * immediately before the jump.
 */

/* per-OS-thread scratch: the context a switch is entering (so a fresh context
   can find its func), and a sink for the discarded continuation of a
   non-returning restore */
static PTH_TLS pth_mctx_t *pth_bctx_self;
static PTH_TLS pth_mctx_t  pth_bctx_trash;

/* entry stub for a freshly made context */
static void pth_bctx_entry(pth_transfer_t tr)
{
    pth_mctx_t *from = (pth_mctx_t *)tr.data;
    from->fc = tr.fctx;              /* record where the switcher suspended  */
    errno = pth_bctx_self->error;    /* start with this context's saved errno */
    (*pth_bctx_self->func)();        /* run our own entry (never returns)    */
    abort();                         /* NOTREACHED */
}

intern void pth_mctx_switch_bctx(pth_mctx_t *old, pth_mctx_t *new)
{
    pth_transfer_t tr;
    /* errno is a per-OS-thread global, not part of the fcontext, so save/restore
       it explicitly across the switch (as the mcsc method does via
       mctx->error); otherwise it would leak between Pth threads */
    old->error = errno;
    /* install the incoming context's real signal mask, saving ours (one
       syscall), exactly as swapcontext(2) would */
    pth_sc(sigprocmask)(SIG_SETMASK, &new->sysmask, &old->sysmask);
    pth_bctx_self = new;
    tr = jump_fcontext(new->fc, old);           /* -> new; resume here later */
    ((pth_mctx_t *)tr.data)->fc = tr.fctx;      /* record resumer's suspension */
    errno = old->error;                         /* restore our errno on resume */
}

intern void pth_mctx_restore_bctx(pth_mctx_t *mctx)
{
    /* jump to mctx and do not return here; the discarded continuation lands
       in the per-thread trash mctx */
    pth_sc(sigprocmask)(SIG_SETMASK, &mctx->sysmask, NULL);
    errno = mctx->error;             /* restore target's errno (mcsc parity) */
    pth_bctx_self = mctx;
    (void)jump_fcontext(mctx->fc, &pth_bctx_trash);
    /* NOTREACHED */
}

intern int pth_mctx_set(
    pth_mctx_t *mctx, void (*func)(void), char *sk_addr_lo, char *sk_addr_hi)
{
    /* fcontext wants the TOP (highest address) of the stack region */
    mctx->fc = make_fcontext((void *)sk_addr_hi,
                             (size_t)(sk_addr_hi - sk_addr_lo),
                             pth_bctx_entry);
    if (mctx->fc == NULL)
        return FALSE;
    mctx->func = func;
    /* capture the current signal mask as this context's initial mask */
    pth_sc(sigprocmask)(SIG_SETMASK, NULL, &mctx->sysmask);
    mctx->error = errno;
    return TRUE;
}

#else
#error "unknown mctx method"
#endif

