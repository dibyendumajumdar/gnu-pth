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
**  pth_sched.c: Pth thread scheduler, the real heart of Pth
*/
                             /* ``Recursive, adj.;
                                  see Recursive.''
                                     -- Unknown   */
/* RTLD_NEXT (used by the MP+emulation OS-thread shim below) needs _GNU_SOURCE
   defined before any system header is pulled in via pth_p.h */
#if defined(PTH_MP) && defined(PTH_EMULATION) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#include "pth_p.h"

#ifdef PTH_MP
/*
 * OS-thread primitives for the auxiliary schedulers. When Pth is built as
 * the POSIX pthread emulation (PTH_EMULATION, i.e. pthread.c is linked in),
 * the pthread_* symbols are the emulation's own, so we must reach the REAL
 * libc primitives via dlsym(RTLD_NEXT). pthread_t is unsigned long on the
 * supported targets, which is exactly what pth_gsched_st.osthread stores.
 */
#if defined(PTH_EMULATION)
#include <dlfcn.h>
#include <signal.h>
typedef int (*pth_osc_fp)(unsigned long *, const void *, void *(*)(void *), void *);
typedef int (*pth_osj_fp)(unsigned long, void **);
typedef int (*pth_oss_fp)(int, const sigset_t *, sigset_t *);
static pth_osc_fp pth_os_create_fp  = NULL;
static pth_osj_fp pth_os_join_fp    = NULL;
static pth_oss_fp pth_os_sigmask_fp = NULL;
static void pth_os_resolve(void)
{
    if (pth_os_create_fp == NULL) {
        pth_os_create_fp  = (pth_osc_fp)dlsym(RTLD_NEXT, "pthread_create");
        pth_os_join_fp    = (pth_osj_fp)dlsym(RTLD_NEXT, "pthread_join");
        pth_os_sigmask_fp = (pth_oss_fp)dlsym(RTLD_NEXT, "pthread_sigmask");
    }
}
#define PTH_OS_CREATE(ptid, fn, arg) (pth_os_resolve(), pth_os_create_fp((ptid), NULL, (fn), (arg)))
#define PTH_OS_JOIN(tid)             (pth_os_resolve(), pth_os_join_fp((tid), NULL))
#define PTH_OS_SIGMASK(how, s, o)    (pth_os_resolve(), pth_os_sigmask_fp((how), (s), (o)))
#else
/* Do NOT pull in <pthread.h> here: in a combined build (core lib + pthread
   emulation) Pth's own pthread.h can shadow the system one and clash. We
   need only three primitives, so declare them directly (pthread_t is
   unsigned long on the supported targets). pthread_sigmask's signature
   matches <signal.h>, so a redundant declaration from there is harmless. */
#include <signal.h>
extern int pthread_create(unsigned long *, const void *, void *(*)(void *), void *);
extern int pthread_join(unsigned long, void **);
extern int pthread_sigmask(int, const sigset_t *, sigset_t *);
#define PTH_OS_CREATE(ptid, fn, arg) pthread_create((ptid), NULL, (fn), (arg))
#define PTH_OS_JOIN(tid)             pthread_join((tid), NULL)
#define PTH_OS_SIGMASK(how, s, o)    pthread_sigmask((how), (s), (o))
#endif
#endif /* PTH_MP */

#if cpp

/*
 * The scheduler object: all per-scheduler state lives here so that
 * multiple schedulers (one per OS thread) can coexist (see
 * MULTISCHED-DESIGN.md). Classic Pth kept this state in globals.
 */
typedef struct pth_gsched_st pth_gsched_t;
struct pth_gsched_st {
    int          id;           /* scheduler identifier                  */
    pth_t        main;         /* the main thread                       */
    pth_t        sched;        /* the permanent scheduler thread        */
    pth_t        current;      /* the currently running thread          */
    pth_pqueue_t NQ;           /* queue of new threads                  */
    pth_pqueue_t RQ;           /* queue of threads ready to run         */
    pth_pqueue_t WQ;           /* queue of threads waiting for an event */
    pth_pqueue_t SQ;           /* queue of suspended threads            */
    pth_pqueue_t DQ;           /* queue of terminated threads           */
    int          favournew;    /* favour new threads on startup         */
    float        loadval;      /* average scheduler load value          */
    int          sigpipe[2];   /* internal signal occurrence pipe       */
    sigset_t     sigpending;   /* mask of pending signals               */
    sigset_t     sigblock;     /* mask of signals we block in scheduler */
    sigset_t     sigcatch;     /* mask of signals we have to catch      */
    sigset_t     sigraised;    /* mask of raised signals                */
    pth_time_t   loadticknext; /* next time point for load calculation  */
    pth_time_t   loadtickgap;  /* interval between load calculations    */
    void * volatile inbox;     /* wakeup inbox: MPSC stack of pth_gmsg_t */
    /* MP control (auxiliary schedulers only) */
    int          stop;         /* shutdown was requested (PTH_GMSG_STOP) */
    pth_mctx_t   bootmctx;     /* OS thread context to return to on stop */
    unsigned long osthread;    /* pthread_t of the scheduler's OS thread */
    pth_atomic_t boot_done;    /* bootstrap finished flag                */
    int          boot_rc;      /* bootstrap result                       */
#if defined(PTH_SCHED_EPOLL) || defined(PTH_SCHED_KQUEUE)
    int          evp_fd;        /* epoll/kqueue instance for this scheduler*/
    struct pth_evreg_st *evp_reg;/* fd-indexed registration cache         */
    int          evp_regcap;    /* slots allocated in evp_reg             */
    unsigned int evp_gen;       /* current event-manager pass generation  */
    int         *evp_touched;   /* fds wanted this pass                    */
    int          evp_ntouched;
    int          evp_touchedcap;
    int         *evp_reglist;   /* fds currently in the kernel interest set*/
    int          evp_nreg;
    int          evp_reglistcap;
    void        *evp_evs;       /* epoll_wait() result buffer             */
    int          evp_evcap;
    int          evp_force;     /* re-assert every registration this pass */
    int          evp_reval_ms;  /* revalidation heartbeat, ms (0 disables)*/
    pth_time_t   evp_nextreval; /* next revalidation time point           */
#endif
};

#define PTH_GSCHED_MAX 64      /* maximum number of schedulers */

/* registry of all schedulers (struct-wrapped so scpp can export it) */
typedef struct { pth_gsched_t *tab[PTH_GSCHED_MAX]; } pth_gsched_tabreg_t;
#define pth_gsched_tab (pth_gsched_tabreg.tab)

/* the active scheduler of the calling OS thread (thread-local in MP builds;
   declared manually because scpp cannot parse the PTH_TLS storage class) */
extern PTH_TLS pth_gsched_t *pth_gsched_active;

/*
 * Single-scheduler source compatibility: the classic global names resolve
 * through the scheduler object of the calling OS thread. Keep using these
 * names in code that means "my scheduler"; code that operates on an
 * explicitly given scheduler should take a pth_gsched_t* instead.
 */
#define pth_main      (pth_gsched_active->main)
#define pth_sched     (pth_gsched_active->sched)
#define pth_current   (pth_gsched_active->current)
#define pth_NQ        (pth_gsched_active->NQ)
#define pth_RQ        (pth_gsched_active->RQ)
#define pth_WQ        (pth_gsched_active->WQ)
#define pth_SQ        (pth_gsched_active->SQ)
#define pth_DQ        (pth_gsched_active->DQ)
#define pth_favournew (pth_gsched_active->favournew)
#define pth_loadval   (pth_gsched_active->loadval)

/*
 * Cross-scheduler message, delivered to a scheduler's wakeup inbox.
 * Allocated by the sender, freed by the draining (home) scheduler.
 */
typedef struct pth_gmsg_st pth_gmsg_t;
struct pth_gmsg_st {
    struct pth_gmsg_st *next;   /* inbox linkage                          */
    int                 kind;   /* PTH_GMSG_*                             */
    pth_t               thread; /* subject thread                         */
    pth_event_t         event;  /* PTH_GMSG_WAKE: event to mark occurred  */
    int                 sig;    /* PTH_GMSG_RAISE: signal number          */
};
#define PTH_GMSG_WAKE   1
#define PTH_GMSG_SPAWN  2
#define PTH_GMSG_STOP   3
#define PTH_GMSG_REAP   4
#define PTH_GMSG_CANCEL 5
#define PTH_GMSG_RAISE  6
#define PTH_GMSG_SUSPEND 7
#define PTH_GMSG_RESUME  8
#define PTH_GMSG_FAVOR   9
#define PTH_GMSG_ABORT   10

#endif /* cpp */

intern pth_gsched_t  pth_gsched_pri;                      /* the primary scheduler            */
PTH_TLS pth_gsched_t *pth_gsched_active = &pth_gsched_pri;/* scheduler of this OS thread      */

intern pth_gsched_tabreg_t pth_gsched_tabreg = { { NULL } }; /* registry of all schedulers */
intern pth_atomic_t  pth_gsched_ntab = PTH_ATOMIC_INIT(1);/* number of schedulers             */
intern pth_atomic_t  pth_gsched_nexited = PTH_ATOMIC_INIT(0); /* auxiliaries that have exited */

/* initialize the scheduler ingredients */
intern int pth_scheduler_init(void)
{
    pth_gsched_t *g = pth_gsched_active;
    pth_time_t gap = PTH_TIME(1,0);

    /* create the internal signal pipe */
    if (pipe(g->sigpipe) == -1)
        return pth_error(FALSE, errno);
    if (pth_fdmode(g->sigpipe[0], PTH_FDMODE_NONBLOCK) == PTH_FDMODE_ERROR)
        return pth_error(FALSE, errno);
    if (pth_fdmode(g->sigpipe[1], PTH_FDMODE_NONBLOCK) == PTH_FDMODE_ERROR)
        return pth_error(FALSE, errno);
#if defined(PTH_SCHED_EPOLL) || defined(PTH_SCHED_KQUEUE)
    if (!pth_evp_init(g))
        return pth_error(FALSE, errno);
#endif

    /* initialize the essential threads */
    g->sched   = NULL;
    g->current = NULL;

    /* initalize the thread queues */
    pth_pqueue_init(&g->NQ);
    pth_pqueue_init(&g->RQ);
    pth_pqueue_init(&g->WQ);
    pth_pqueue_init(&g->SQ);
    pth_pqueue_init(&g->DQ);

    /* initialize scheduling hints */
    g->favournew = 1; /* the default is the original behaviour */

    /* initialize load support */
    g->loadval = 1.0;
    pth_time_set(&g->loadtickgap, &gap);
    pth_time_set(&g->loadticknext, PTH_TIME_NOW);

    /* initialize the wakeup inbox */
    g->inbox = NULL;

    /* initialize MP control state */
    g->stop = FALSE;

    return TRUE;
}

/* drop all threads (except for the currently active one) */
intern void pth_scheduler_drop(void)
{
    pth_gsched_t *g = pth_gsched_active;
    pth_t t;

    /* clear the new queue */
    while ((t = pth_pqueue_delmax(&g->NQ)) != NULL)
        pth_tcb_free(t);
    pth_pqueue_init(&g->NQ);

    /* clear the ready queue */
    while ((t = pth_pqueue_delmax(&g->RQ)) != NULL)
        pth_tcb_free(t);
    pth_pqueue_init(&g->RQ);

    /* clear the waiting queue */
    while ((t = pth_pqueue_delmax(&g->WQ)) != NULL)
        pth_tcb_free(t);
    pth_pqueue_init(&g->WQ);

    /* clear the suspend queue */
    while ((t = pth_pqueue_delmax(&g->SQ)) != NULL)
        pth_tcb_free(t);
    pth_pqueue_init(&g->SQ);

    /* clear the dead queue */
    while ((t = pth_pqueue_delmax(&g->DQ)) != NULL)
        pth_tcb_free(t);
    pth_pqueue_init(&g->DQ);
    return;
}

/* kill the scheduler ingredients */
intern void pth_scheduler_kill(void)
{
    pth_gsched_t *g = pth_gsched_active;

    /* drop all threads */
    pth_scheduler_drop();

    /* remove the internal signal pipe */
#if defined(PTH_SCHED_EPOLL) || defined(PTH_SCHED_KQUEUE)
    pth_evp_kill(g);
#endif
    close(g->sigpipe[0]);
    close(g->sigpipe[1]);
    return;
}
/*
 * Wake a thread parked on a synchronization primitive: deliver a message
 * to its home scheduler's wakeup inbox. If that scheduler is a foreign
 * one it may currently be blocked in select(2), so we additionally kick
 * its signal pipe. Our own scheduler needs no kick because it always
 * drains the inbox before going to sleep. NOTE the ordering: the message
 * is pushed BEFORE the pipe byte is written; the event manager clears
 * the pipe BEFORE draining, so no wakeup can be lost.
 */
intern int pth_gsched_post(pth_gsched_t *g, int kind, pth_t t, pth_event_t ev)
{
    pth_gmsg_t *m;
    void *head;

    if ((m = (pth_gmsg_t *)malloc(sizeof(pth_gmsg_t))) == NULL)
        return FALSE;
    m->kind   = kind;
    m->thread = t;
    m->event  = ev;
    m->sig    = 0;
    do {
        head = g->inbox;
        m->next = (pth_gmsg_t *)head;
    } while (!pth_atomic_ptr_cas(&g->inbox, head, m));
    if (g != pth_gsched_active)
        pth_sc(write)(g->sigpipe[1], "!", 1);
    return TRUE;
}

intern int pth_gsched_wake(pth_gsched_t *g, pth_t t, pth_event_t ev)
{
    return pth_gsched_post(g, PTH_GMSG_WAKE, t, ev);
}

/* like pth_gsched_post but carries a signal number for PTH_GMSG_RAISE */
intern int pth_gsched_post_raise(pth_gsched_t *g, pth_t t, int sig)
{
    pth_gmsg_t *m;
    void *head;

    if ((m = (pth_gmsg_t *)malloc(sizeof(pth_gmsg_t))) == NULL)
        return FALSE;
    m->kind   = PTH_GMSG_RAISE;
    m->thread = t;
    m->event  = NULL;
    m->sig    = sig;
    do {
        head = g->inbox;
        m->next = (pth_gmsg_t *)head;
    } while (!pth_atomic_ptr_cas(&g->inbox, head, m));
    if (g != pth_gsched_active)
        pth_sc(write)(g->sigpipe[1], "!", 1);
    return TRUE;
}

/*
 * Nudge every scheduler other than the caller's by writing a byte to its
 * signal pipe, so a scheduler asleep in poll(2)/select(2) wakes up and
 * re-evaluates its waiting threads. Used by pth_msgport_put(3): message
 * ports are polled (not on a directed-wakeup waiter queue), so a remote
 * put must break the target scheduler out of its sleep. No inbox message
 * is posted; the wakeup byte alone triggers a fresh event-manager pass.
 */
intern void pth_gsched_kick_others(void)
{
#ifdef PTH_MP
    pth_gsched_t *self = pth_gsched_active;
    int i, n = pth_atomic_load(&pth_gsched_ntab);
    for (i = 0; i < n; i++) {
        pth_gsched_t *o = (i == 0) ? &pth_gsched_pri : pth_gsched_tab[i];
        if (o != NULL && o != self)
            pth_sc(write)(o->sigpipe[1], "m", 1);
    }
#endif
    return;
}

/*
 * Drain the wakeup inbox: apply all pending messages. Returns TRUE if
 * any message was processed. Only ever runs in the scheduler's own OS
 * thread, so all thread/event/queue manipulation stays scheduler-local.
 */
intern int pth_gsched_drain(pth_gsched_t *g)
{
    pth_gmsg_t *m, *next, *rev;
    int any = FALSE;

    m = (pth_gmsg_t *)pth_atomic_ptr_xchg(&g->inbox, NULL);
    /* reverse the LIFO push order to obtain FIFO fairness */
    rev = NULL;
    while (m != NULL) {
        next = m->next;
        m->next = rev;
        rev = m;
        m = next;
    }
    while (rev != NULL) {
        next = rev->next;
        if (rev->kind == PTH_GMSG_WAKE) {
            /* mark the event as occurred; the event manager's cleanup
               loop then moves the thread from the waiting queue to the
               ready queue. A stale wakeup (waiter timed out and moved
               on concurrently) at worst marks a static event that is
               reset at its next use, which is harmless. */
            if (rev->event != NULL)
                rev->event->ev_status = PTH_STATUS_OCCURRED;
        }
        else if (rev->kind == PTH_GMSG_SPAWN) {
            /* adopt a remotely spawned thread into our new queue */
#if PTH_MCTX_MTH(mcsc)
            /* the machine context was created on the spawner's OS thread
               and carries its signal mask; auxiliary schedulers run with
               all signals blocked (signals are scheduler 0 business) */
            if (g->id != 0)
                sigfillset(&rev->thread->mctx.uc.uc_sigmask);
#elif PTH_MCTX_MTH(bctx)
            if (g->id != 0)
                sigfillset(&rev->thread->mctx.sysmask);
#endif
            rev->thread->state = PTH_STATE_NEW;
            pth_pqueue_insert(&g->NQ, rev->thread->prio, rev->thread);
        }
        else if (rev->kind == PTH_GMSG_STOP) {
            /* shut down as soon as we have run dry */
            g->stop = TRUE;
        }
        else if (rev->kind == PTH_GMSG_REAP) {
            /* a cross-scheduler joiner has read the return value and asked
               us (the home scheduler) to free the dead TCB. The thread was
               placed in our DQ when it died, before this message could be
               processed, so it is always found here. */
            pth_pqueue_delete(&g->DQ, rev->thread);
            pth_tcb_free(rev->thread);
        }
        else if (rev->kind == PTH_GMSG_CANCEL) {
            /* carry out a cross-scheduler cancellation locally: we own this
               thread's TCB and run queues (see pth_cancel.c) */
            (void)pth_cancel_local(rev->thread);
        }
        else if (rev->kind == PTH_GMSG_RAISE) {
            /* deliver a cross-scheduler raised signal locally (see pth_lib.c) */
            pth_raise_local(rev->thread, rev->sig);
        }
        else if (rev->kind == PTH_GMSG_SUSPEND) {
            /* suspend a thread we own (see pth_lib.c) */
            (void)pth_suspend_local(rev->thread);
        }
        else if (rev->kind == PTH_GMSG_RESUME) {
            /* resume a thread we own (see pth_lib.c) */
            (void)pth_resume_local(rev->thread);
        }
        else if (rev->kind == PTH_GMSG_FAVOR) {
            /* yield-to hint: bump a thread we own to the front of its queue */
            pth_favor_local(rev->thread);
        }
        else if (rev->kind == PTH_GMSG_ABORT) {
            /* force-abort a thread we own (detach + async cancel; see pth_cancel.c) */
            pth_abort_local(rev->thread);
        }
        free(rev);
        any = TRUE;
        rev = next;
    }
    return any;
}

/*
 * Update the average scheduler load.
 *
 * This is called on every context switch, but we have to adjust the
 * average load value every second, only. If we're called more than
 * once per second we handle this by just calculating anything once
 * and then do NOPs until the next ticks is over. If the scheduler
 * waited for more than once second (or a thread CPU burst lasted for
 * more than once second) we simulate the missing calculations. That's
 * no problem because we can assume that the number of ready threads
 * then wasn't changed dramatically (or more context switched would have
 * been occurred and we would have been given more chances to operate).
 * The actual average load is calculated through an exponential average
 * formula.
 */
#define pth_scheduler_load(g, now) \
    if (pth_time_cmp((now), &g->loadticknext) >= 0) { \
        pth_time_t ttmp; \
        int numready; \
        numready = pth_pqueue_elements(&g->RQ); \
        pth_time_set(&ttmp, (now)); \
        do { \
            g->loadval = (numready*0.25) + (g->loadval*0.75); \
            pth_time_sub(&ttmp, &g->loadtickgap); \
        } while (pth_time_cmp(&ttmp, &g->loadticknext) >= 0); \
        pth_time_set(&g->loadticknext, (now)); \
        pth_time_add(&g->loadticknext, &g->loadtickgap); \
    }

/* the heart of this library: the thread scheduler */
intern void *pth_scheduler(void *dummy)
{
    pth_gsched_t *g = pth_gsched_active;
    sigset_t sigs;
    pth_time_t running;
    pth_time_t snapshot;
    struct sigaction sa;
    sigset_t ss;
    int sig;
    pth_t t;

    /*
     * bootstrapping
     */
    pth_debug1("pth_scheduler: bootstrapping");

    /* mark this thread as the special scheduler thread */
    g->sched->state = PTH_STATE_SCHEDULER;

    /* block all signals in the scheduler thread */
    sigfillset(&sigs);
    pth_sc(sigprocmask)(SIG_SETMASK, &sigs, NULL);

    /* initialize the snapshot time for bootstrapping the loop */
    pth_time_set(&snapshot, PTH_TIME_NOW);

    /*
     * endless scheduler loop
     */
    for (;;) {
        /*
         * Move threads from new queue to ready queue and optionally
         * give them maximum priority so they start immediately.
         */
        while ((t = pth_pqueue_tail(&g->NQ)) != NULL) {
            pth_pqueue_delete(&g->NQ, t);
            t->state = PTH_STATE_READY;
            if (g->favournew)
                pth_pqueue_insert(&g->RQ, pth_pqueue_favorite_prio(&g->RQ), t);
            else
                pth_pqueue_insert(&g->RQ, PTH_PRIO_STD, t);
            pth_debug2("pth_scheduler: new thread \"%s\" moved to top of ready queue", t->name);
        }

        /*
         * Update average scheduler load
         */
        pth_scheduler_load(g, &snapshot);

        /*
         * Find next thread in ready queue
         */
        g->current = pth_pqueue_delmax(&g->RQ);
        if (g->current == NULL) {
#ifdef PTH_MP
            if (g->id != 0) {
                /* an auxiliary scheduler may legitimately run dry: shut
                   down if requested and fully drained, else wait for new
                   work to arrive through the wakeup inbox */
                if (   g->stop
                    && pth_pqueue_elements(&g->NQ) == 0
                    && pth_pqueue_elements(&g->WQ) == 0
                    && pth_pqueue_elements(&g->SQ) == 0)
                    pth_mctx_switch(&g->sched->mctx, &g->bootmctx);
                pth_time_set(&snapshot, PTH_TIME_NOW);
                pth_sched_eventmanager(&snapshot, FALSE /* wait */);
                continue;
            }
#endif
            /* The ready queue is empty. If threads are still parked waiting
               for events (e.g. a cross-scheduler message-port arrival, which
               only wakes us via a signal-pipe kick and is re-evaluated on the
               event manager's next assembly pass), keep waiting instead of
               treating this as a fatal condition. Only a truly empty set of
               queues is an internal error. */
            if (   pth_pqueue_elements(&g->WQ) > 0
                || pth_pqueue_elements(&g->NQ) > 0
                || pth_pqueue_elements(&g->SQ) > 0) {
                pth_time_set(&snapshot, PTH_TIME_NOW);
                pth_sched_eventmanager(&snapshot, FALSE /* wait */);
                continue;
            }
            fprintf(stderr, "**Pth** SCHEDULER INTERNAL ERROR: "
                            "no more thread(s) available to schedule!?!?\n");
            abort();
        }
        pth_debug4("pth_scheduler: thread \"%s\" selected (prio=%d, qprio=%d)",
                   g->current->name, g->current->prio, g->current->q_prio);

        /*
         * Raise additionally thread-specific signals
         * (they are delivered when we switch the context)
         *
         * Situation is ('#' = signal pending):
         *     process pending (g->sigpending):         ----####
         *     thread pending (g->current->sigpending): --##--##
         * Result has to be:
         *     process new pending:                      --######
         */
        if (g->current->sigpendcnt > 0) {
            sigpending(&g->sigpending);
            for (sig = 1; sig < PTH_NSIG; sig++)
                if (sigismember(&g->current->sigpending, sig))
                    if (!sigismember(&g->sigpending, sig))
                        kill(getpid(), sig);
        }

        /*
         * Set running start time for new thread
         * and perform a context switch to it
         */
        pth_debug3("pth_scheduler: switching to thread 0x%lx (\"%s\")",
                   (unsigned long)g->current, g->current->name);

        /* update thread times */
        pth_time_set(&g->current->lastran, PTH_TIME_NOW);

        /* update scheduler times */
        pth_time_set(&running, &g->current->lastran);
        pth_time_sub(&running, &snapshot);
        pth_time_add(&g->sched->running, &running);

        /* ** ENTERING THREAD ** - by switching the machine context */
        g->current->dispatches++;
        pth_mctx_switch(&g->sched->mctx, &g->current->mctx);

        /* update scheduler times */
        pth_time_set(&snapshot, PTH_TIME_NOW);
        pth_debug3("pth_scheduler: cameback from thread 0x%lx (\"%s\")",
                   (unsigned long)g->current, g->current->name);

        /*
         * Calculate and update the time the previous thread was running
         */
        pth_time_set(&running, &snapshot);
        pth_time_sub(&running, &g->current->lastran);
        pth_time_add(&g->current->running, &running);
        pth_debug3("pth_scheduler: thread \"%s\" ran %.6f",
                   g->current->name, pth_time_t2d(&running));

        /*
         * Remove still pending thread-specific signals
         * (they are re-delivered next time)
         *
         * Situation is ('#' = signal pending):
         *     thread old pending (g->current->sigpending): --##--##
         *     process old pending (g->sigpending):         ----####
         *     process still pending (sigstillpending):      ---#-#-#
         * Result has to be:
         *     process new pending:                          -----#-#
         *     thread new pending (g->current->sigpending): ---#---#
         */
        if (g->current->sigpendcnt > 0) {
            sigset_t sigstillpending;
            sigpending(&sigstillpending);
            for (sig = 1; sig < PTH_NSIG; sig++) {
                if (sigismember(&g->current->sigpending, sig)) {
                    if (!sigismember(&sigstillpending, sig)) {
                        /* thread (and perhaps also process) signal delivered */
                        sigdelset(&g->current->sigpending, sig);
                        g->current->sigpendcnt--;
                    }
                    else if (!sigismember(&g->sigpending, sig)) {
                        /* thread signal not delivered */
                        pth_util_sigdelete(sig);
                    }
                }
            }
        }

        /*
         * Check for stack overflow
         */
        if (g->current->stackguard != NULL) {
            if (*g->current->stackguard != 0xDEAD) {
                pth_debug3("pth_scheduler: stack overflow detected for thread 0x%lx (\"%s\")",
                           (unsigned long)g->current, g->current->name);
                /*
                 * if the application doesn't catch SIGSEGVs, we terminate
                 * manually with a SIGSEGV now, but output a reasonable message.
                 */
                if (sigaction(SIGSEGV, NULL, &sa) == 0) {
                    if (sa.sa_handler == SIG_DFL) {
                        fprintf(stderr, "**Pth** STACK OVERFLOW: thread pid_t=0x%lx, name=\"%s\"\n",
                                (unsigned long)g->current, g->current->name);
                        kill(getpid(), SIGSEGV);
                        sigfillset(&ss);
                        sigdelset(&ss, SIGSEGV);
                        sigsuspend(&ss);
                        abort();
                    }
                }
                /*
                 * else we terminate the thread only and send us a SIGSEGV
                 * which allows the application to handle the situation...
                 */
                g->current->join_arg = (void *)0xDEAD;
                g->current->state = PTH_STATE_DEAD;
                kill(getpid(), SIGSEGV);
            }
        }

        /*
         * If previous thread is now marked as dead, kick it out
         */
        if (g->current->state == PTH_STATE_DEAD) {
            pth_debug2("pth_scheduler: marking thread \"%s\" as dead", g->current->name);
            if (!g->current->joinable)
                pth_tcb_free(g->current);
            else {
                pth_t jt = g->current;
                pth_t jw;
                /* Keep the dead TCB in our DQ (local pth_join / join-any
                   reap it from here, and so does a remote reap message).
                   Then publish death to any parked cross-scheduler joiners
                   and deliver them a directed wakeup; each will read
                   join_arg and post PTH_GMSG_REAP back to us to free it. */
                pth_pqueue_insert(&g->DQ, PTH_PRIO_STD, jt);
                pth_spin_lock(&jt->join_lock);
                jt->join_done = TRUE;
                while ((jw = pth_waitq_shift(&jt->join_waitq)) != NULL)
                    pth_gsched_wake(jw->sched_home, jw, jw->wq_event);
                pth_spin_unlock(&jt->join_lock);
            }
            g->current = NULL;
        }

        /*
         * If thread wants to wait for an event
         * move it to waiting queue now
         */
        if (g->current != NULL && g->current->state == PTH_STATE_WAITING) {
            pth_debug2("pth_scheduler: moving thread \"%s\" to waiting queue",
                       g->current->name);
            pth_pqueue_insert(&g->WQ, g->current->prio, g->current);
            g->current = NULL;
        }

        /*
         * migrate old treads in ready queue into higher
         * priorities to avoid starvation and insert last running
         * thread back into this queue, too.
         */
        pth_pqueue_increase(&g->RQ);
        if (g->current != NULL)
            pth_pqueue_insert(&g->RQ, g->current->prio, g->current);

        /*
         * Manage the events in the waiting queue, i.e. decide whether their
         * events occurred and move them to the ready queue. But wait only if
         * we have already no new or ready threads.
         */
        if (   pth_pqueue_elements(&g->RQ) == 0
            && pth_pqueue_elements(&g->NQ) == 0) {
#ifdef PTH_MP
            /* before an auxiliary scheduler blocks waiting for new work:
               if shutdown was requested and it has fully run dry, leave
               (the STOP wakeup byte was already consumed, so blocking in
               select(2) here would sleep forever) */
            if (   g->id != 0
                && g->stop
                && pth_pqueue_elements(&g->WQ) == 0
                && pth_pqueue_elements(&g->SQ) == 0)
                pth_mctx_switch(&g->sched->mctx, &g->bootmctx);
#endif
            /* still no NEW or READY threads, so we have to wait for new work */
            pth_sched_eventmanager(&snapshot, FALSE /* wait */);
        }
        else
            /* already NEW or READY threads exists, so just poll for even more work */
            pth_sched_eventmanager(&snapshot, TRUE  /* poll */);
    }

    /* NOTREACHED */
    return NULL;
}

/*
 * Look whether some events already occurred (or failed) and move
 * corresponding threads from waiting queue back to ready queue.
 */
#ifdef PTH_SCHED_POLL
#include <poll.h>

/*
 * poll(2)-based fd readiness for the scheduler core (prototype, opt-in via
 * -DPTH_SCHED_POLL). Unlike select(2)/fd_set it imposes no FD_SETSIZE ceiling
 * on the descriptor VALUES a thread may wait on. The pollfd vector is grown
 * on demand and reused across passes; entries for the same fd are merged.
 */
static int pth_pollset_add(struct pollfd **pv, int *pn, int *pmax, int fd, short ev)
{
    int i;
    for (i = 0; i < *pn; i++) {
        if ((*pv)[i].fd == fd) {
            (*pv)[i].events |= ev;
            return i;
        }
    }
    if (*pn >= *pmax) {
        int nm = (*pmax == 0 ? 16 : *pmax * 2);
        struct pollfd *np = (struct pollfd *)realloc(*pv, nm * sizeof(struct pollfd));
        if (np == NULL)
            return -1;
        *pv = np;
        *pmax = nm;
    }
    (*pv)[*pn].fd      = fd;
    (*pv)[*pn].events  = ev;
    (*pv)[*pn].revents = 0;
    return (*pn)++;
}

static short pth_pollset_revents(struct pollfd *pv, int pn, int fd)
{
    int i;
    for (i = 0; i < pn; i++)
        if (pv[i].fd == fd)
            return pv[i].revents;
    return 0;
}
#endif /* PTH_SCHED_POLL */

#ifdef PTH_SCHED_POLL
intern void pth_sched_eventmanager_poll(pth_time_t *now, int dopoll)
{
    pth_gsched_t *g = pth_gsched_active;
    pth_t nexttimer_thread;
    pth_event_t nexttimer_ev;
    pth_time_t nexttimer_value;
    pth_event_t evh;
    pth_event_t ev;
    pth_t t;
    pth_t tlast;
    int this_occurred;
    int any_occurred;
    struct pollfd *pfd;
    int npfd;
    int pfdmax;
    struct timeval delay;
    struct timeval *pdelay;
    sigset_t oss;
    struct sigaction sa;
    struct sigaction osa[1+PTH_NSIG];
    char minibuf[128];
    int loop_repeat;
    int rc;
    int sig;
    int n;

    pth_debug2("pth_sched_eventmanager: enter in %s mode",
               dopoll ? "polling" : "waiting");
    pfd = NULL; npfd = 0; pfdmax = 0;

    /* entry point for internal looping in event handling */
    loop_entry:
    loop_repeat = FALSE;

    /* clear the signal pipe and drain the wakeup inbox first: an already
       delivered wakeup must prevent us from blocking in select(2) below
       (producers push their message before writing the pipe byte) */
    while (pth_sc(read)(g->sigpipe[0], minibuf, sizeof(minibuf)) > 0) ;
    if (pth_gsched_drain(g))
        dopoll = TRUE;

    /* initialize fd sets */
    npfd = 0;

    /* initialize signal status */
    sigpending(&g->sigpending);
    sigfillset(&g->sigblock);
    sigemptyset(&g->sigcatch);
    sigemptyset(&g->sigraised);

    /* initialize next timer */
    pth_time_set(&nexttimer_value, PTH_TIME_ZERO);
    nexttimer_thread = NULL;
    nexttimer_ev = NULL;

    /* for all threads in the waiting queue... */
    any_occurred = FALSE;
    for (t = pth_pqueue_head(&g->WQ); t != NULL;
         t = pth_pqueue_walk(&g->WQ, t, PTH_WALK_NEXT)) {

        /* determine signals we block */
        for (sig = 1; sig < PTH_NSIG; sig++)
            if (!sigismember(&(t->mctx.sigs), sig))
                sigdelset(&g->sigblock, sig);

        /* cancellation support */
        if (t->cancelreq == TRUE)
            any_occurred = TRUE;

        /* ... and all their events... */
        if (t->events == NULL)
            continue;
        /* ...check whether events occurred */
        ev = evh = t->events;
        do {
            if (ev->ev_status == PTH_STATUS_PENDING) {
                this_occurred = FALSE;

                /* Filedescriptor I/O */
                if (ev->ev_type == PTH_EVENT_FD) {
                    /* filedescriptors are checked later all at once.
                       Here we only assemble them in the fd sets */
                    short pe = 0;
                    if (ev->ev_goal & PTH_UNTIL_FD_READABLE)  pe |= POLLIN;
                    if (ev->ev_goal & PTH_UNTIL_FD_WRITEABLE) pe |= POLLOUT;
                    if (ev->ev_goal & PTH_UNTIL_FD_EXCEPTION) pe |= POLLPRI;
                    pth_pollset_add(&pfd, &npfd, &pfdmax, ev->ev_args.FD.fd, pe);
                }
                /* Filedescriptor Set Select I/O */
                else if (ev->ev_type == PTH_EVENT_SELECT) {
                    /* filedescriptors are checked later all at once.
                       Here we only merge the fd sets. */
                    int sfd;
                    for (sfd = 0; sfd < ev->ev_args.SELECT.nfd; sfd++) {
                        short pe = 0;
                        if (ev->ev_args.SELECT.rfds && FD_ISSET(sfd, ev->ev_args.SELECT.rfds)) pe |= POLLIN;
                        if (ev->ev_args.SELECT.wfds && FD_ISSET(sfd, ev->ev_args.SELECT.wfds)) pe |= POLLOUT;
                        if (ev->ev_args.SELECT.efds && FD_ISSET(sfd, ev->ev_args.SELECT.efds)) pe |= POLLPRI;
                        if (pe != 0)
                            pth_pollset_add(&pfd, &npfd, &pfdmax, sfd, pe);
                    }
                }
                /* Signal Set */
                else if (ev->ev_type == PTH_EVENT_SIGS) {
                    for (sig = 1; sig < PTH_NSIG; sig++) {
                        if (sigismember(ev->ev_args.SIGS.sigs, sig)) {
                            /* thread signal handling */
                            if (sigismember(&t->sigpending, sig)) {
                                *(ev->ev_args.SIGS.sig) = sig;
                                sigdelset(&t->sigpending, sig);
                                t->sigpendcnt--;
                                this_occurred = TRUE;
                            }
                            /* process signal handling */
                            if (sigismember(&g->sigpending, sig)) {
                                if (ev->ev_args.SIGS.sig != NULL)
                                    *(ev->ev_args.SIGS.sig) = sig;
                                pth_util_sigdelete(sig);
                                sigdelset(&g->sigpending, sig);
                                this_occurred = TRUE;
                            }
                            else {
                                sigdelset(&g->sigblock, sig);
                                sigaddset(&g->sigcatch, sig);
                            }
                        }
                    }
                }
                /* Timer */
                else if (ev->ev_type == PTH_EVENT_TIME) {
                    if (pth_time_cmp(&(ev->ev_args.TIME.tv), now) < 0)
                        this_occurred = TRUE;
                    else {
                        /* remember the timer which will be elapsed next */
                        if ((nexttimer_thread == NULL && nexttimer_ev == NULL) ||
                            pth_time_cmp(&(ev->ev_args.TIME.tv), &nexttimer_value) < 0) {
                            nexttimer_thread = t;
                            nexttimer_ev = ev;
                            pth_time_set(&nexttimer_value, &(ev->ev_args.TIME.tv));
                        }
                    }
                }
                /* Message Port Arrivals */
                else if (ev->ev_type == PTH_EVENT_MSG) {
                    /* mp_queue is shared with cross-scheduler pth_msgport_put;
                       read the predicate under the port's spinlock */
                    pth_spin_lock(&ev->ev_args.MSG.mp->mp_lock);
                    if (pth_ring_elements(&(ev->ev_args.MSG.mp->mp_queue)) > 0)
                        this_occurred = TRUE;
                    pth_spin_unlock(&ev->ev_args.MSG.mp->mp_lock);
                }
                /* Mutex Release and Condition Variable Signal events
                   are not polled anymore: threads park on the
                   primitive's wait queue and are woken explicitly
                   through the scheduler's wakeup inbox (pth_sync.c) */
                /* Thread Termination */
                else if (ev->ev_type == PTH_EVENT_TID) {
                    if (   (   ev->ev_args.TID.tid == NULL
                            && pth_pqueue_elements(&g->DQ) > 0)
                        || (   ev->ev_args.TID.tid != NULL
                            && ev->ev_args.TID.tid->state == ev->ev_goal))
                        this_occurred = TRUE;
                }
                /* Custom Event Function */
                else if (ev->ev_type == PTH_EVENT_FUNC) {
                    if (ev->ev_args.FUNC.func(ev->ev_args.FUNC.arg))
                        this_occurred = TRUE;
                    else {
                        pth_time_t tv;
                        pth_time_set(&tv, now);
                        pth_time_add(&tv, &(ev->ev_args.FUNC.tv));
                        if ((nexttimer_thread == NULL && nexttimer_ev == NULL) ||
                            pth_time_cmp(&tv, &nexttimer_value) < 0) {
                            nexttimer_thread = t;
                            nexttimer_ev = ev;
                            pth_time_set(&nexttimer_value, &tv);
                        }
                    }
                }

                /* tag event if it has occurred */
                if (this_occurred) {
                    pth_debug2("pth_sched_eventmanager: [non-I/O] event occurred for thread \"%s\"", t->name);
                    ev->ev_status = PTH_STATUS_OCCURRED;
                    any_occurred = TRUE;
                }
            }
        } while ((ev = ev->ev_next) != evh);
    }
    if (any_occurred)
        dopoll = TRUE;

    /* now decide how to poll for fd I/O and timers */
    if (dopoll) {
        /* do a polling with immediate timeout,
           i.e. check the fd sets only without blocking */
        pth_time_set(&delay, PTH_TIME_ZERO);
        pdelay = &delay;
    }
    else if (nexttimer_ev != NULL) {
        /* do a polling with a timeout set to the next timer,
           i.e. wait for the fd sets or the next timer */
        pth_time_set(&delay, &nexttimer_value);
        pth_time_sub(&delay, now);
        pdelay = &delay;
    }
    else {
        /* do a polling without a timeout,
           i.e. wait for the fd sets only with blocking */
        pdelay = NULL;
    }

    /* let poll() wait for the read-part of the signal pipe
       (it was already cleared at the top of this event loop) */
    pth_pollset_add(&pfd, &npfd, &pfdmax, g->sigpipe[0], POLLIN);

    /* replace signal actions for signals we've to catch for events
       (signal handling is the business of scheduler 0 only; auxiliary
       schedulers keep all signals permanently blocked) */
    if (g->id == 0) {
        for (sig = 1; sig < PTH_NSIG; sig++) {
            if (sigismember(&g->sigcatch, sig)) {
                sa.sa_handler = pth_sched_eventmanager_sighandler;
                sigfillset(&sa.sa_mask);
                sa.sa_flags = 0;
                sigaction(sig, &sa, &osa[sig]);
            }
        }

        /* allow some signals to be delivered: Either to our
           catching handler or directly to the configured
           handler for signals not catched by events */
        pth_sc(sigprocmask)(SIG_SETMASK, &g->sigblock, &oss);
    }

    /* now do the polling for filedescriptor I/O and timers
       WHEN THE SCHEDULER SLEEPS AT ALL, THEN HERE!! */
    rc = -1;
    {
        int ptimeout;
        if (dopoll)
            ptimeout = 0;
        else if (pdelay != NULL) {
            ptimeout = (int)(pdelay->tv_sec*1000 + (pdelay->tv_usec+999)/1000);
            if (ptimeout < 0)
                ptimeout = 0;
        }
        else
            ptimeout = -1;   /* block indefinitely */
        if (!(dopoll && npfd == 0))
            while ((rc = pth_sc(poll)(pfd, npfd, ptimeout)) < 0
                   && errno == EINTR) ;
    }

    /* restore signal mask and actions and handle signals */
    if (g->id == 0) {
        pth_sc(sigprocmask)(SIG_SETMASK, &oss, NULL);
        for (sig = 1; sig < PTH_NSIG; sig++)
            if (sigismember(&g->sigcatch, sig))
                sigaction(sig, &osa[sig], NULL);
    }

    /* drain the wakeup inbox again: messages which arrived while we
       were sleeping in select(2) must be applied before the cleanup
       loop below moves threads out of the waiting queue */
    pth_gsched_drain(g);

    /* if the timer elapsed, handle it */
    if (!dopoll && rc == 0 && nexttimer_ev != NULL) {
        if (nexttimer_ev->ev_type == PTH_EVENT_FUNC) {
            /* it was an implicit timer event for a function event,
               so repeat the event handling for rechecking the function */
            loop_repeat = TRUE;
        }
        else {
            /* it was an explicit timer event, standing for its own */
            pth_debug2("pth_sched_eventmanager: [timeout] event occurred for thread \"%s\"",
                       nexttimer_thread->name);
            nexttimer_ev->ev_status = PTH_STATUS_OCCURRED;
        }
    }

    /* if the internal signal pipe was used, adjust the poll() results */
    if (!dopoll && rc > 0 && (pth_pollset_revents(pfd, npfd, g->sigpipe[0]) & POLLIN))
        rc--;

    /* on timeout nothing is ready; on error keep the per-fd revents
       (POLLNVAL/POLLERR pinpoint the offending descriptors) */
    if (rc == 0) {
        int _i;
        for (_i = 0; _i < npfd; _i++)
            pfd[_i].revents = 0;
    }

    /* now comes the final cleanup loop where we've to
       do two jobs: first we've to do the late handling of the fd I/O events and
       additionally if a thread has one occurred event, we move it from the
       waiting queue to the ready queue */

    /* for all threads in the waiting queue... */
    t = pth_pqueue_head(&g->WQ);
    while (t != NULL) {

        /* do the late handling of the fd I/O and signal
           events in the waiting event ring */
        any_occurred = FALSE;
        if (t->events != NULL) {
            ev = evh = t->events;
            do {
                /*
                 * Late handling for still not occured events
                 */
                if (ev->ev_status == PTH_STATUS_PENDING) {
                    /* Filedescriptor I/O */
                    if (ev->ev_type == PTH_EVENT_FD) {
                        short re = pth_pollset_revents(pfd, npfd, ev->ev_args.FD.fd);
                        if (   ((ev->ev_goal & PTH_UNTIL_FD_READABLE)  && (re & (POLLIN|POLLHUP|POLLERR)))
                            || ((ev->ev_goal & PTH_UNTIL_FD_WRITEABLE) && (re & (POLLOUT|POLLERR|POLLHUP)))
                            || ((ev->ev_goal & PTH_UNTIL_FD_EXCEPTION) && (re & POLLPRI)) ) {
                            pth_debug2("pth_sched_eventmanager: "
                                       "[I/O] event occurred for thread \"%s\"", t->name);
                            ev->ev_status = PTH_STATUS_OCCURRED;
                        }
                        else if (re & POLLNVAL) {
                            /* poll pinpoints a bad descriptor directly */
                            ev->ev_status = PTH_STATUS_FAILED;
                            pth_debug2("pth_sched_eventmanager: "
                                       "[I/O] event failed for thread \"%s\"", t->name);
                        }
                    }
                    /* Filedescriptor Set I/O */
                    else if (ev->ev_type == PTH_EVENT_SELECT) {
                        fd_set trfds, twfds, tefds;
                        int sfd, bad = FALSE;
                        FD_ZERO(&trfds); FD_ZERO(&twfds); FD_ZERO(&tefds);
                        for (sfd = 0; sfd < ev->ev_args.SELECT.nfd; sfd++) {
                            short re = pth_pollset_revents(pfd, npfd, sfd);
                            if (re == 0)
                                continue;
                            if (re & POLLNVAL)
                                bad = TRUE;
                            if ((re & (POLLIN|POLLHUP|POLLERR)) && ev->ev_args.SELECT.rfds
                                && FD_ISSET(sfd, ev->ev_args.SELECT.rfds))  FD_SET(sfd, &trfds);
                            if ((re & (POLLOUT|POLLERR|POLLHUP)) && ev->ev_args.SELECT.wfds
                                && FD_ISSET(sfd, ev->ev_args.SELECT.wfds))  FD_SET(sfd, &twfds);
                            if ((re & POLLPRI) && ev->ev_args.SELECT.efds
                                && FD_ISSET(sfd, ev->ev_args.SELECT.efds))  FD_SET(sfd, &tefds);
                        }
                        if (pth_util_fds_test(ev->ev_args.SELECT.nfd,
                                              ev->ev_args.SELECT.rfds, &trfds,
                                              ev->ev_args.SELECT.wfds, &twfds,
                                              ev->ev_args.SELECT.efds, &tefds)) {
                            n = pth_util_fds_select(ev->ev_args.SELECT.nfd,
                                                    ev->ev_args.SELECT.rfds, &trfds,
                                                    ev->ev_args.SELECT.wfds, &twfds,
                                                    ev->ev_args.SELECT.efds, &tefds);
                            if (ev->ev_args.SELECT.n != NULL)
                                *(ev->ev_args.SELECT.n) = n;
                            ev->ev_status = PTH_STATUS_OCCURRED;
                            pth_debug2("pth_sched_eventmanager: "
                                       "[I/O] event occurred for thread \"%s\"", t->name);
                        }
                        else if (bad) {
                            ev->ev_status = PTH_STATUS_FAILED;
                            pth_debug2("pth_sched_eventmanager: "
                                       "[I/O] event failed for thread \"%s\"", t->name);
                        }
                    }
                    /* Signal Set */
                    else if (ev->ev_type == PTH_EVENT_SIGS) {
                        for (sig = 1; sig < PTH_NSIG; sig++) {
                            if (sigismember(ev->ev_args.SIGS.sigs, sig)) {
                                if (sigismember(&g->sigraised, sig)) {
                                    if (ev->ev_args.SIGS.sig != NULL)
                                        *(ev->ev_args.SIGS.sig) = sig;
                                    pth_debug2("pth_sched_eventmanager: "
                                               "[signal] event occurred for thread \"%s\"", t->name);
                                    sigdelset(&g->sigraised, sig);
                                    ev->ev_status = PTH_STATUS_OCCURRED;
                                }
                            }
                        }
                    }
                }
                /* (no post-processing of already occurred events is
                   needed anymore: the condition variable signal flag
                   machinery is gone) */

                /* local to global mapping */
                if (ev->ev_status != PTH_STATUS_PENDING)
                    any_occurred = TRUE;
            } while ((ev = ev->ev_next) != evh);
        }

        /* cancellation support */
        if (t->cancelreq == TRUE) {
            pth_debug2("pth_sched_eventmanager: cancellation request pending for thread \"%s\"", t->name);
            any_occurred = TRUE;
        }

        /* walk to next thread in waiting queue */
        tlast = t;
        t = pth_pqueue_walk(&g->WQ, t, PTH_WALK_NEXT);

        /*
         * move last thread to ready queue if any events occurred for it.
         * we insert it with a slightly increased queue priority to it a
         * better chance to immediately get scheduled, else the last running
         * thread might immediately get again the CPU which is usually not
         * what we want, because we oven use pth_yield() calls to give others
         * a chance.
         */
        if (any_occurred) {
            pth_pqueue_delete(&g->WQ, tlast);
            tlast->state = PTH_STATE_READY;
            pth_pqueue_insert(&g->RQ, tlast->prio+1, tlast);
            pth_debug2("pth_sched_eventmanager: thread \"%s\" moved from waiting "
                       "to ready queue", tlast->name);
        }
    }

    /* perhaps we have to internally loop... */
    if (loop_repeat) {
        pth_time_set(now, PTH_TIME_NOW);
        goto loop_entry;
    }

    if (pfd != NULL)
        free(pfd);
    pth_debug1("pth_sched_eventmanager: leaving");
    return;
}

#endif /* PTH_SCHED_POLL */

#if defined(PTH_SCHED_EPOLL) || defined(PTH_SCHED_KQUEUE)
/*
 * Persistent-registration fd readiness for the scheduler core (opt-in via
 * -DPTH_SCHED_EPOLL on Linux or -DPTH_SCHED_KQUEUE on *BSD/macOS; either flag
 * also switches on the poll(2) fast paths). Where select(2)/poll(2) rebuild and
 * rescan the whole descriptor set on every scheduler pass, epoll/kqueue keep a
 * *persistent* interest set in the kernel: a scheduler with N idle waiters no
 * longer costs an N-descriptor kernel scan each pass -- the wait returns only
 * the ready descriptors. Each scheduler owns its own epoll/kqueue instance plus
 * an fd-indexed registration cache diffed against the waiting queue each pass,
 * so only genuine changes reach the kernel. The event manager below
 * (pth_sched_eventmanager_evport) is backend-agnostic; only the small pth_evp_*
 * primitives (commit/wait/osfd) differ between epoll and kqueue. See
 * MULTISCHED-DESIGN.md section 23.
 */
#include <fcntl.h>
#if defined(PTH_SCHED_EPOLL)
#include <sys/epoll.h>
#elif defined(PTH_SCHED_KQUEUE)
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#endif

#define PTH_EVP_R 0x1
#define PTH_EVP_W 0x2
#define PTH_EVP_E 0x4
#define PTH_EVP_FAIL 0x8   /* registration failed (bad descriptor) */

struct pth_evreg_st {
    unsigned int  gen;        /* pass this fd was last wanted in            */
    unsigned int  rdygen;     /* pass this fd's readiness was last set in   */
    short         want;       /* PTH_EVP_* mask wanted this pass            */
    short         regmask;    /* PTH_EVP_* mask currently in the kernel     */
    short         ready;      /* PTH_EVP_* readiness from this pass         */
    unsigned char registered; /* currently in the kernel interest set       */
};

/* ---- generic registration-cache bookkeeping (backend independent) ------- */

static int pth_evp_grow_reg(pth_gsched_t *g, int fd)
{
    int nc, i;
    struct pth_evreg_st *nr;
    if (fd < g->evp_regcap)
        return TRUE;
    nc = (g->evp_regcap == 0 ? 64 : g->evp_regcap);
    while (nc <= fd)
        nc *= 2;
    nr = (struct pth_evreg_st *)realloc(g->evp_reg, nc * sizeof(struct pth_evreg_st));
    if (nr == NULL)
        return FALSE;
    for (i = g->evp_regcap; i < nc; i++) {
        nr[i].gen = 0; nr[i].rdygen = 0;
        nr[i].want = 0; nr[i].regmask = 0;
        nr[i].ready = 0; nr[i].registered = 0;
    }
    g->evp_reg = nr;
    g->evp_regcap = nc;
    return TRUE;
}

static int pth_evp_push(int **v, int *n, int *cap, int fd)
{
    if (*n >= *cap) {
        int nc = (*cap == 0 ? 32 : *cap * 2);
        int *nv = (int *)realloc(*v, nc * sizeof(int));
        if (nv == NULL)
            return FALSE;
        *v = nv; *cap = nc;
    }
    (*v)[(*n)++] = fd;
    return TRUE;
}

static void pth_evp_begin(pth_gsched_t *g)
{
    g->evp_gen++;
    g->evp_ntouched = 0;
    g->evp_force = FALSE;
    if (g->evp_gen == 0) {           /* generation wrapped: invalidate cache */
        int i;
        for (i = 0; i < g->evp_regcap; i++) {
            g->evp_reg[i].gen = 0;
            g->evp_reg[i].rdygen = 0;
        }
        g->evp_gen = 1;
    }
}

static int pth_evp_want(pth_gsched_t *g, int fd, short mask)
{
    struct pth_evreg_st *r;
    if (fd < 0)
        return FALSE;
    if (!pth_evp_grow_reg(g, fd))
        return FALSE;
    r = &g->evp_reg[fd];
    if (r->gen != g->evp_gen) {       /* first time this fd is wanted this pass */
        r->gen  = g->evp_gen;
        r->want = mask;
        return pth_evp_push(&g->evp_touched, &g->evp_ntouched, &g->evp_touchedcap, fd);
    }
    r->want |= mask;
    return TRUE;
}

static short pth_evp_ready(pth_gsched_t *g, int fd)
{
    if (fd >= 0 && fd < g->evp_regcap && g->evp_reg[fd].rdygen == g->evp_gen)
        return g->evp_reg[fd].ready;
    return 0;
}

#if defined(PTH_SCHED_EPOLL)
/* ---- epoll(7) backend --------------------------------------------------- */

static int pth_evp_to_epoll(short m)
{
    int e = 0;
    if (m & PTH_EVP_R) e |= EPOLLIN;
    if (m & PTH_EVP_W) e |= EPOLLOUT;
    if (m & PTH_EVP_E) e |= EPOLLPRI;
    return e;
}

static short pth_evp_from_epoll(int e)
{
    short m = 0;
    if (e & (EPOLLIN |EPOLLHUP|EPOLLERR)) m |= PTH_EVP_R;
    if (e & (EPOLLOUT|EPOLLHUP|EPOLLERR)) m |= PTH_EVP_W;
    if (e & EPOLLPRI)                     m |= PTH_EVP_E;
    return m;
}

static void pth_evp_ctl(pth_gsched_t *g, int fd, struct pth_evreg_st *r)
{
    struct epoll_event ee;
    memset(&ee, 0, sizeof(ee));
    ee.events  = pth_evp_to_epoll(r->want);
    ee.data.fd = fd;
    if (!r->registered) {
        if (epoll_ctl(g->evp_fd, EPOLL_CTL_ADD, fd, &ee) == 0) {
            r->registered = 1;
            r->regmask = r->want;
            pth_evp_push(&g->evp_reglist, &g->evp_nreg, &g->evp_reglistcap, fd);
        }
        else if (errno == EEXIST) {
            /* kernel still holds a stale registration we lost track of */
            if (epoll_ctl(g->evp_fd, EPOLL_CTL_MOD, fd, &ee) == 0) {
                r->registered = 1;
                r->regmask = r->want;
                pth_evp_push(&g->evp_reglist, &g->evp_nreg, &g->evp_reglistcap, fd);
            }
        }
        else {
            /* bad descriptor at registration: flag FAILED so pth_select() and
               pth_read()/pth_write() wake with an error (the poll(2) POLLNVAL
               semantics) instead of blocking forever */
            r->ready  = PTH_EVP_FAIL;
            r->rdygen = g->evp_gen;
        }
    }
    else if (r->regmask != r->want || g->evp_force) {
        if (epoll_ctl(g->evp_fd, EPOLL_CTL_MOD, fd, &ee) == 0)
            r->regmask = r->want;
        else if (errno == ENOENT) {
            /* descriptor was closed under us and the number reused: the kernel
               dropped the old registration, so add the new one afresh */
            if (epoll_ctl(g->evp_fd, EPOLL_CTL_ADD, fd, &ee) == 0)
                r->regmask = r->want;
            else {
                r->registered = 0; r->regmask = 0;
                r->ready = PTH_EVP_FAIL; r->rdygen = g->evp_gen;
            }
        }
        else {
            /* the descriptor is gone -- e.g. closed while still being waited on
               -- so the kernel silently dropped it. Drop the stale cache entry
               and wake the waiter with FAILED. Caught on the revalidation pass
               (g->evp_force) even when nothing else changed. */
            r->registered = 0; r->regmask = 0;
            r->ready = PTH_EVP_FAIL; r->rdygen = g->evp_gen;
        }
    }
}

static void pth_evp_commit(pth_gsched_t *g)
{
    int i;
    for (i = 0; i < g->evp_ntouched; i++) {
        int fd = g->evp_touched[i];
        pth_evp_ctl(g, fd, &g->evp_reg[fd]);
    }
    for (i = 0; i < g->evp_nreg; ) {
        int fd = g->evp_reglist[i];
        struct pth_evreg_st *r = &g->evp_reg[fd];
        if (r->gen != g->evp_gen || !r->registered) {
            if (r->registered)
                epoll_ctl(g->evp_fd, EPOLL_CTL_DEL, fd, NULL);
            r->registered = 0;
            r->regmask = 0;
            g->evp_reglist[i] = g->evp_reglist[--g->evp_nreg];
        }
        else
            i++;
    }
}

static int pth_evp_wait(pth_gsched_t *g, int timeout_ms)
{
    struct epoll_event *evs;
    int n, i;
    if (g->evp_nreg + 1 > g->evp_evcap) {
        int nc = (g->evp_evcap == 0 ? 64 : g->evp_evcap);
        struct epoll_event *ne;
        while (nc < g->evp_nreg + 1)
            nc *= 2;
        ne = (struct epoll_event *)realloc(g->evp_evs, nc * sizeof(struct epoll_event));
        if (ne != NULL) { g->evp_evs = ne; g->evp_evcap = nc; }
    }
    if (g->evp_evcap == 0)
        return 0;
    while ((n = epoll_wait(g->evp_fd, (struct epoll_event *)g->evp_evs,
                           g->evp_evcap, timeout_ms)) < 0 && errno == EINTR) ;
    if (n > 0) {
        evs = (struct epoll_event *)g->evp_evs;
        for (i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            if (fd >= 0 && fd < g->evp_regcap) {
                g->evp_reg[fd].ready  = pth_evp_from_epoll(evs[i].events);
                g->evp_reg[fd].rdygen = g->evp_gen;
            }
        }
    }
    return n;
}

static int pth_evp_osfd(void)
{
    int fd = epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0) {
        /* older kernels without epoll_create1(2) */
        fd = epoll_create(256);
        if (fd >= 0)
            fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    return fd;
}

#elif defined(PTH_SCHED_KQUEUE)
/* ---- kqueue(2) backend (*BSD / macOS) ----------------------------------- */

/* submit a batch of changes; absorb EV_ERROR (mark bad fds ready so the
   waiter's own I/O returns the error) and stamp any readiness delivered while
   applying (kqueue is level-triggered, so it would be re-reported anyway) */
static void pth_evp_kq_apply(pth_gsched_t *g, struct kevent *ch, int nch)
{
    struct kevent res[128];
    struct timespec zero;
    int n, k, fd;
    short bit;
    if (nch <= 0)
        return;
    zero.tv_sec = 0; zero.tv_nsec = 0;
    n = kevent(g->evp_fd, ch, nch, res, (int)(sizeof(res)/sizeof(res[0])), &zero);
    if (n <= 0)
        return;
    for (k = 0; k < n; k++) {
        fd = (int)res[k].ident;
        if (fd < 0 || fd >= g->evp_regcap)
            continue;
        if (res[k].flags & EV_ERROR) {
            /* data==0 is a success ack; ENOENT is a benign delete-miss */
            if (res[k].data == 0 || res[k].data == ENOENT)
                continue;
            /* the descriptor is gone (bad fd, or closed while waited on):
               flag FAILED (POLLNVAL parity) and drop the dead registration */
            bit = PTH_EVP_FAIL;
            g->evp_reg[fd].registered = 0;
            g->evp_reg[fd].regmask = 0;
        }
        else {
            bit = 0;
            if (res[k].filter == EVFILT_READ)  bit |= PTH_EVP_R;
            else if (res[k].filter == EVFILT_WRITE) bit |= PTH_EVP_W;
#ifdef EVFILT_EXCEPT
            else if (res[k].filter == EVFILT_EXCEPT) bit |= PTH_EVP_E;
#endif
            if (res[k].flags & EV_EOF) bit |= (PTH_EVP_R|PTH_EVP_W);
        }
        if (g->evp_reg[fd].rdygen != g->evp_gen) {
            g->evp_reg[fd].ready = 0;
            g->evp_reg[fd].rdygen = g->evp_gen;
        }
        g->evp_reg[fd].ready |= bit;
    }
}

static void pth_evp_kq_push(pth_gsched_t *g, struct kevent *ch, int *nch,
                            int fd, int filter, int flags)
{
    if (*nch >= 64) {
        pth_evp_kq_apply(g, ch, *nch);
        *nch = 0;
    }
    EV_SET(&ch[*nch], fd, filter, flags, 0, 0, NULL);
    (*nch)++;
}

static void pth_evp_commit(pth_gsched_t *g)
{
    struct kevent ch[64];
    int nch = 0, i, fd;
    struct pth_evreg_st *r;
    short want, have;
    /* ADD/DELETE individual filters for everything wanted this pass */
    for (i = 0; i < g->evp_ntouched; i++) {
        fd = g->evp_touched[i];
        r  = &g->evp_reg[fd];
        want = r->want;
        have = r->registered ? r->regmask : 0;
        if ( (want & PTH_EVP_R) && (!(have & PTH_EVP_R) || g->evp_force)) pth_evp_kq_push(g, ch, &nch, fd, EVFILT_READ,  EV_ADD);
        if (!(want & PTH_EVP_R) &&  (have & PTH_EVP_R)) pth_evp_kq_push(g, ch, &nch, fd, EVFILT_READ,  EV_DELETE);
        if ( (want & PTH_EVP_W) && (!(have & PTH_EVP_W) || g->evp_force)) pth_evp_kq_push(g, ch, &nch, fd, EVFILT_WRITE, EV_ADD);
        if (!(want & PTH_EVP_W) &&  (have & PTH_EVP_W)) pth_evp_kq_push(g, ch, &nch, fd, EVFILT_WRITE, EV_DELETE);
#ifdef EVFILT_EXCEPT
        if ( (want & PTH_EVP_E) && (!(have & PTH_EVP_E) || g->evp_force)) pth_evp_kq_push(g, ch, &nch, fd, EVFILT_EXCEPT, EV_ADD);
        if (!(want & PTH_EVP_E) &&  (have & PTH_EVP_E)) pth_evp_kq_push(g, ch, &nch, fd, EVFILT_EXCEPT, EV_DELETE);
#endif
        if (!r->registered) {
            r->registered = 1;
            pth_evp_push(&g->evp_reglist, &g->evp_nreg, &g->evp_reglistcap, fd);
        }
        r->regmask = want;
    }
    /* DELETE every filter for fds no longer wanted (swap-remove from reglist) */
    for (i = 0; i < g->evp_nreg; ) {
        fd = g->evp_reglist[i];
        r  = &g->evp_reg[fd];
        if (r->gen != g->evp_gen || !r->registered) {
            if (r->regmask & PTH_EVP_R) pth_evp_kq_push(g, ch, &nch, fd, EVFILT_READ,  EV_DELETE);
            if (r->regmask & PTH_EVP_W) pth_evp_kq_push(g, ch, &nch, fd, EVFILT_WRITE, EV_DELETE);
#ifdef EVFILT_EXCEPT
            if (r->regmask & PTH_EVP_E) pth_evp_kq_push(g, ch, &nch, fd, EVFILT_EXCEPT, EV_DELETE);
#endif
            r->registered = 0;
            r->regmask = 0;
            g->evp_reglist[i] = g->evp_reglist[--g->evp_nreg];
        }
        else
            i++;
    }
    pth_evp_kq_apply(g, ch, nch);
}

static int pth_evp_wait(pth_gsched_t *g, int timeout_ms)
{
    struct kevent *evs;
    struct timespec ts, *pts;
    int n, i, fd, need;
    short bit;
    need = g->evp_nreg * 3 + 1;   /* up to R+W+E filters per registered fd */
    if (need > g->evp_evcap) {
        int nc = (g->evp_evcap == 0 ? 64 : g->evp_evcap);
        struct kevent *ne;
        while (nc < need)
            nc *= 2;
        ne = (struct kevent *)realloc(g->evp_evs, nc * sizeof(struct kevent));
        if (ne != NULL) { g->evp_evs = ne; g->evp_evcap = nc; }
    }
    if (g->evp_evcap == 0)
        return 0;
    if (timeout_ms < 0)
        pts = NULL;
    else {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        pts = &ts;
    }
    while ((n = kevent(g->evp_fd, NULL, 0, (struct kevent *)g->evp_evs,
                       g->evp_evcap, pts)) < 0 && errno == EINTR) ;
    if (n > 0) {
        evs = (struct kevent *)g->evp_evs;
        for (i = 0; i < n; i++) {
            fd  = (int)evs[i].ident;
            bit = 0;
            if (evs[i].filter == EVFILT_READ)  bit |= PTH_EVP_R;
            else if (evs[i].filter == EVFILT_WRITE) bit |= PTH_EVP_W;
#ifdef EVFILT_EXCEPT
            else if (evs[i].filter == EVFILT_EXCEPT) bit |= PTH_EVP_E;
#endif
            if (evs[i].flags & EV_EOF) bit |= (PTH_EVP_R|PTH_EVP_W);
            if (fd >= 0 && fd < g->evp_regcap) {
                if (g->evp_reg[fd].rdygen != g->evp_gen) {
                    g->evp_reg[fd].ready = 0;
                    g->evp_reg[fd].rdygen = g->evp_gen;
                }
                g->evp_reg[fd].ready |= bit;
            }
        }
    }
    return n;
}

static int pth_evp_osfd(void)
{
    int fd = kqueue();
    if (fd >= 0)
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

#endif /* backend */

/* ---- generic instance lifecycle ----------------------------------------- */

intern int pth_evp_init(pth_gsched_t *g)
{
    g->evp_fd = pth_evp_osfd();
    if (g->evp_fd < 0)
        return FALSE;
    g->evp_reg = NULL;     g->evp_regcap = 0;      g->evp_gen = 0;
    g->evp_touched = NULL; g->evp_ntouched = 0;    g->evp_touchedcap = 0;
    g->evp_reglist = NULL; g->evp_nreg = 0;        g->evp_reglistcap = 0;
    g->evp_evs = NULL;     g->evp_evcap = 0;
    g->evp_force = FALSE;
    {
        /* bounded revalidation heartbeat: periodically re-assert every
           registration so a descriptor closed while a thread is still
           waiting on it is detected and the waiter woken with EBADF,
           rather than stranded (persistent registration cannot otherwise
           notice a silent kernel deregistration). 0 disables it. */
        const char *e = getenv("PTH_SCHED_REVAL_MS");
        g->evp_reval_ms = (e != NULL ? atoi(e) : 1000);
    }
    pth_time_set(&g->evp_nextreval, PTH_TIME_ZERO);
    return TRUE;
}

intern void pth_evp_kill(pth_gsched_t *g)
{
    if (g->evp_fd >= 0)
        close(g->evp_fd);
    g->evp_fd = -1;
    if (g->evp_reg     != NULL) { free(g->evp_reg);     g->evp_reg = NULL; }
    if (g->evp_touched != NULL) { free(g->evp_touched); g->evp_touched = NULL; }
    if (g->evp_reglist != NULL) { free(g->evp_reglist); g->evp_reglist = NULL; }
    if (g->evp_evs     != NULL) { free(g->evp_evs);     g->evp_evs = NULL; }
    g->evp_regcap = g->evp_touchedcap = g->evp_reglistcap = g->evp_evcap = 0;
    g->evp_ntouched = g->evp_nreg = 0;
}

intern void pth_sched_eventmanager_evport(pth_time_t *now, int dopoll)
{
    pth_gsched_t *g = pth_gsched_active;
    pth_t nexttimer_thread;
    pth_event_t nexttimer_ev;
    pth_time_t nexttimer_value;
    pth_event_t evh;
    pth_event_t ev;
    pth_t t;
    pth_t tlast;
    int this_occurred;
    int any_occurred;
    struct timeval delay;
    struct timeval *pdelay;
    sigset_t oss;
    struct sigaction sa;
    struct sigaction osa[1+PTH_NSIG];
    char minibuf[128];
    int loop_repeat;
    int rc;
    int sig;
    int n;

    pth_debug2("pth_sched_eventmanager: enter in %s mode",
               dopoll ? "polling" : "waiting");

    /* entry point for internal looping in event handling */
    loop_entry:
    loop_repeat = FALSE;
    pth_evp_begin(g);

    /* clear the signal pipe and drain the wakeup inbox first: an already
       delivered wakeup must prevent us from blocking in select(2) below
       (producers push their message before writing the pipe byte) */
    while (pth_sc(read)(g->sigpipe[0], minibuf, sizeof(minibuf)) > 0) ;
    if (pth_gsched_drain(g))
        dopoll = TRUE;


    /* initialize signal status */
    sigpending(&g->sigpending);
    sigfillset(&g->sigblock);
    sigemptyset(&g->sigcatch);
    sigemptyset(&g->sigraised);

    /* initialize next timer */
    pth_time_set(&nexttimer_value, PTH_TIME_ZERO);
    nexttimer_thread = NULL;
    nexttimer_ev = NULL;

    /* for all threads in the waiting queue... */
    any_occurred = FALSE;
    for (t = pth_pqueue_head(&g->WQ); t != NULL;
         t = pth_pqueue_walk(&g->WQ, t, PTH_WALK_NEXT)) {

        /* determine signals we block */
        for (sig = 1; sig < PTH_NSIG; sig++)
            if (!sigismember(&(t->mctx.sigs), sig))
                sigdelset(&g->sigblock, sig);

        /* cancellation support */
        if (t->cancelreq == TRUE)
            any_occurred = TRUE;

        /* ... and all their events... */
        if (t->events == NULL)
            continue;
        /* ...check whether events occurred */
        ev = evh = t->events;
        do {
            if (ev->ev_status == PTH_STATUS_PENDING) {
                this_occurred = FALSE;

                /* Filedescriptor I/O */
                if (ev->ev_type == PTH_EVENT_FD) {
                    /* filedescriptors are checked later all at once.
                       Here we only assemble them in the fd sets */
                    short pe = 0;
                    if (ev->ev_goal & PTH_UNTIL_FD_READABLE)  pe |= PTH_EVP_R;
                    if (ev->ev_goal & PTH_UNTIL_FD_WRITEABLE) pe |= PTH_EVP_W;
                    if (ev->ev_goal & PTH_UNTIL_FD_EXCEPTION) pe |= PTH_EVP_E;
                    pth_evp_want(g, ev->ev_args.FD.fd, pe);
                }
                /* Filedescriptor Set Select I/O */
                else if (ev->ev_type == PTH_EVENT_SELECT) {
                    /* filedescriptors are checked later all at once.
                       Here we only merge the fd sets. */
                    int sfd;
                    for (sfd = 0; sfd < ev->ev_args.SELECT.nfd; sfd++) {
                        short pe = 0;
                        if (ev->ev_args.SELECT.rfds && FD_ISSET(sfd, ev->ev_args.SELECT.rfds)) pe |= PTH_EVP_R;
                        if (ev->ev_args.SELECT.wfds && FD_ISSET(sfd, ev->ev_args.SELECT.wfds)) pe |= PTH_EVP_W;
                        if (ev->ev_args.SELECT.efds && FD_ISSET(sfd, ev->ev_args.SELECT.efds)) pe |= PTH_EVP_E;
                        if (pe != 0)
                            pth_evp_want(g, sfd, pe);
                    }
                }
                /* Signal Set */
                else if (ev->ev_type == PTH_EVENT_SIGS) {
                    for (sig = 1; sig < PTH_NSIG; sig++) {
                        if (sigismember(ev->ev_args.SIGS.sigs, sig)) {
                            /* thread signal handling */
                            if (sigismember(&t->sigpending, sig)) {
                                *(ev->ev_args.SIGS.sig) = sig;
                                sigdelset(&t->sigpending, sig);
                                t->sigpendcnt--;
                                this_occurred = TRUE;
                            }
                            /* process signal handling */
                            if (sigismember(&g->sigpending, sig)) {
                                if (ev->ev_args.SIGS.sig != NULL)
                                    *(ev->ev_args.SIGS.sig) = sig;
                                pth_util_sigdelete(sig);
                                sigdelset(&g->sigpending, sig);
                                this_occurred = TRUE;
                            }
                            else {
                                sigdelset(&g->sigblock, sig);
                                sigaddset(&g->sigcatch, sig);
                            }
                        }
                    }
                }
                /* Timer */
                else if (ev->ev_type == PTH_EVENT_TIME) {
                    if (pth_time_cmp(&(ev->ev_args.TIME.tv), now) < 0)
                        this_occurred = TRUE;
                    else {
                        /* remember the timer which will be elapsed next */
                        if ((nexttimer_thread == NULL && nexttimer_ev == NULL) ||
                            pth_time_cmp(&(ev->ev_args.TIME.tv), &nexttimer_value) < 0) {
                            nexttimer_thread = t;
                            nexttimer_ev = ev;
                            pth_time_set(&nexttimer_value, &(ev->ev_args.TIME.tv));
                        }
                    }
                }
                /* Message Port Arrivals */
                else if (ev->ev_type == PTH_EVENT_MSG) {
                    /* mp_queue is shared with cross-scheduler pth_msgport_put;
                       read the predicate under the port's spinlock */
                    pth_spin_lock(&ev->ev_args.MSG.mp->mp_lock);
                    if (pth_ring_elements(&(ev->ev_args.MSG.mp->mp_queue)) > 0)
                        this_occurred = TRUE;
                    pth_spin_unlock(&ev->ev_args.MSG.mp->mp_lock);
                }
                /* Mutex Release and Condition Variable Signal events
                   are not polled anymore: threads park on the
                   primitive's wait queue and are woken explicitly
                   through the scheduler's wakeup inbox (pth_sync.c) */
                /* Thread Termination */
                else if (ev->ev_type == PTH_EVENT_TID) {
                    if (   (   ev->ev_args.TID.tid == NULL
                            && pth_pqueue_elements(&g->DQ) > 0)
                        || (   ev->ev_args.TID.tid != NULL
                            && ev->ev_args.TID.tid->state == ev->ev_goal))
                        this_occurred = TRUE;
                }
                /* Custom Event Function */
                else if (ev->ev_type == PTH_EVENT_FUNC) {
                    if (ev->ev_args.FUNC.func(ev->ev_args.FUNC.arg))
                        this_occurred = TRUE;
                    else {
                        pth_time_t tv;
                        pth_time_set(&tv, now);
                        pth_time_add(&tv, &(ev->ev_args.FUNC.tv));
                        if ((nexttimer_thread == NULL && nexttimer_ev == NULL) ||
                            pth_time_cmp(&tv, &nexttimer_value) < 0) {
                            nexttimer_thread = t;
                            nexttimer_ev = ev;
                            pth_time_set(&nexttimer_value, &tv);
                        }
                    }
                }

                /* tag event if it has occurred */
                if (this_occurred) {
                    pth_debug2("pth_sched_eventmanager: [non-I/O] event occurred for thread \"%s\"", t->name);
                    ev->ev_status = PTH_STATUS_OCCURRED;
                    any_occurred = TRUE;
                }
            }
        } while ((ev = ev->ev_next) != evh);
    }
    if (any_occurred)
        dopoll = TRUE;

    /* now decide how to poll for fd I/O and timers */
    if (dopoll) {
        /* do a polling with immediate timeout,
           i.e. check the fd sets only without blocking */
        pth_time_set(&delay, PTH_TIME_ZERO);
        pdelay = &delay;
    }
    else if (nexttimer_ev != NULL) {
        /* do a polling with a timeout set to the next timer,
           i.e. wait for the fd sets or the next timer */
        pth_time_set(&delay, &nexttimer_value);
        pth_time_sub(&delay, now);
        pdelay = &delay;
    }
    else {
        /* do a polling without a timeout,
           i.e. wait for the fd sets only with blocking */
        pdelay = NULL;
    }

    /* let poll() wait for the read-part of the signal pipe
       (it was already cleared at the top of this event loop) */
    pth_evp_want(g, g->sigpipe[0], PTH_EVP_R);
    if (g->evp_reval_ms > 0 && pth_time_cmp(now, &g->evp_nextreval) >= 0) {
        pth_time_t _iv = pth_time(g->evp_reval_ms / 1000,
                                  (long)(g->evp_reval_ms % 1000) * 1000);
        g->evp_force = TRUE;                    /* re-assert all this pass */
        pth_time_set(&g->evp_nextreval, now);
        pth_time_add(&g->evp_nextreval, &_iv);
    }
    pth_evp_commit(g);

    /* replace signal actions for signals we've to catch for events
       (signal handling is the business of scheduler 0 only; auxiliary
       schedulers keep all signals permanently blocked) */
    if (g->id == 0) {
        for (sig = 1; sig < PTH_NSIG; sig++) {
            if (sigismember(&g->sigcatch, sig)) {
                sa.sa_handler = pth_sched_eventmanager_sighandler;
                sigfillset(&sa.sa_mask);
                sa.sa_flags = 0;
                sigaction(sig, &sa, &osa[sig]);
            }
        }

        /* allow some signals to be delivered: Either to our
           catching handler or directly to the configured
           handler for signals not catched by events */
        pth_sc(sigprocmask)(SIG_SETMASK, &g->sigblock, &oss);
    }

    /* now do the polling for filedescriptor I/O and timers
       WHEN THE SCHEDULER SLEEPS AT ALL, THEN HERE!! */
    rc = -1;
    {
        int ptimeout;
        if (dopoll)
            ptimeout = 0;
        else if (pdelay != NULL) {
            ptimeout = (int)(pdelay->tv_sec*1000 + (pdelay->tv_usec+999)/1000);
            if (ptimeout < 0)
                ptimeout = 0;
        }
        else
            ptimeout = -1;   /* block indefinitely */
        /* never block past the revalidation heartbeat, so a scheduler that is
           otherwise idle still periodically re-checks its registrations */
        if (g->evp_reval_ms > 0 && (ptimeout < 0 || ptimeout > g->evp_reval_ms))
            ptimeout = g->evp_reval_ms;
        rc = pth_evp_wait(g, ptimeout);
    }

    /* restore signal mask and actions and handle signals */
    if (g->id == 0) {
        pth_sc(sigprocmask)(SIG_SETMASK, &oss, NULL);
        for (sig = 1; sig < PTH_NSIG; sig++)
            if (sigismember(&g->sigcatch, sig))
                sigaction(sig, &osa[sig], NULL);
    }

    /* drain the wakeup inbox again: messages which arrived while we
       were sleeping in select(2) must be applied before the cleanup
       loop below moves threads out of the waiting queue */
    pth_gsched_drain(g);

    /* if the timer elapsed, handle it */
    if (!dopoll && rc == 0 && nexttimer_ev != NULL) {
        if (nexttimer_ev->ev_type == PTH_EVENT_FUNC) {
            /* it was an implicit timer event for a function event,
               so repeat the event handling for rechecking the function */
            loop_repeat = TRUE;
        }
        else {
            /* it was an explicit timer event, standing for its own */
            pth_debug2("pth_sched_eventmanager: [timeout] event occurred for thread \"%s\"",
                       nexttimer_thread->name);
            nexttimer_ev->ev_status = PTH_STATUS_OCCURRED;
        }
    }

    /* if the internal signal pipe was used, adjust the poll() results */
    if (!dopoll && rc > 0 && (pth_evp_ready(g, g->sigpipe[0]) & PTH_EVP_R))
        rc--;

    /* on timeout/error nothing is ready (fd readiness is generation-stamped) */

    /* now comes the final cleanup loop where we've to
       do two jobs: first we've to do the late handling of the fd I/O events and
       additionally if a thread has one occurred event, we move it from the
       waiting queue to the ready queue */

    /* for all threads in the waiting queue... */
    t = pth_pqueue_head(&g->WQ);
    while (t != NULL) {

        /* do the late handling of the fd I/O and signal
           events in the waiting event ring */
        any_occurred = FALSE;
        if (t->events != NULL) {
            ev = evh = t->events;
            do {
                /*
                 * Late handling for still not occured events
                 */
                if (ev->ev_status == PTH_STATUS_PENDING) {
                    /* Filedescriptor I/O */
                    if (ev->ev_type == PTH_EVENT_FD) {
                        short re = pth_evp_ready(g, ev->ev_args.FD.fd);
                        if (   ((ev->ev_goal & PTH_UNTIL_FD_READABLE)  && (re & PTH_EVP_R))
                            || ((ev->ev_goal & PTH_UNTIL_FD_WRITEABLE) && (re & PTH_EVP_W))
                            || ((ev->ev_goal & PTH_UNTIL_FD_EXCEPTION) && (re & PTH_EVP_E)) ) {
                            pth_debug2("pth_sched_eventmanager: "
                                       "[I/O] event occurred for thread \"%s\"", t->name);
                            ev->ev_status = PTH_STATUS_OCCURRED;
                        }
                        else if (re & PTH_EVP_FAIL) {
                            /* registration failed (bad descriptor) */
                            ev->ev_status = PTH_STATUS_FAILED;
                        }
                    }
                    /* Filedescriptor Set I/O */
                    else if (ev->ev_type == PTH_EVENT_SELECT) {
                        fd_set trfds, twfds, tefds;
                        int sfd, bad = FALSE;
                        FD_ZERO(&trfds); FD_ZERO(&twfds); FD_ZERO(&tefds);
                        for (sfd = 0; sfd < ev->ev_args.SELECT.nfd; sfd++) {
                            short re = pth_evp_ready(g, sfd);
                            if (re == 0)
                                continue;
                            if (re & PTH_EVP_FAIL)
                                bad = TRUE;
                            if ((re & PTH_EVP_R) && ev->ev_args.SELECT.rfds
                                && FD_ISSET(sfd, ev->ev_args.SELECT.rfds))  FD_SET(sfd, &trfds);
                            if ((re & PTH_EVP_W) && ev->ev_args.SELECT.wfds
                                && FD_ISSET(sfd, ev->ev_args.SELECT.wfds))  FD_SET(sfd, &twfds);
                            if ((re & PTH_EVP_E) && ev->ev_args.SELECT.efds
                                && FD_ISSET(sfd, ev->ev_args.SELECT.efds))  FD_SET(sfd, &tefds);
                        }
                        if (pth_util_fds_test(ev->ev_args.SELECT.nfd,
                                              ev->ev_args.SELECT.rfds, &trfds,
                                              ev->ev_args.SELECT.wfds, &twfds,
                                              ev->ev_args.SELECT.efds, &tefds)) {
                            n = pth_util_fds_select(ev->ev_args.SELECT.nfd,
                                                    ev->ev_args.SELECT.rfds, &trfds,
                                                    ev->ev_args.SELECT.wfds, &twfds,
                                                    ev->ev_args.SELECT.efds, &tefds);
                            if (ev->ev_args.SELECT.n != NULL)
                                *(ev->ev_args.SELECT.n) = n;
                            ev->ev_status = PTH_STATUS_OCCURRED;
                            pth_debug2("pth_sched_eventmanager: "
                                       "[I/O] event occurred for thread \"%s\"", t->name);
                        }
                        else if (bad) {
                            /* only invalid descriptors were seen: fail the select */
                            ev->ev_status = PTH_STATUS_FAILED;
                            pth_debug2("pth_sched_eventmanager: "
                                       "[I/O] event failed for thread \"%s\"", t->name);
                        }
                    }
                    /* Signal Set */
                    else if (ev->ev_type == PTH_EVENT_SIGS) {
                        for (sig = 1; sig < PTH_NSIG; sig++) {
                            if (sigismember(ev->ev_args.SIGS.sigs, sig)) {
                                if (sigismember(&g->sigraised, sig)) {
                                    if (ev->ev_args.SIGS.sig != NULL)
                                        *(ev->ev_args.SIGS.sig) = sig;
                                    pth_debug2("pth_sched_eventmanager: "
                                               "[signal] event occurred for thread \"%s\"", t->name);
                                    sigdelset(&g->sigraised, sig);
                                    ev->ev_status = PTH_STATUS_OCCURRED;
                                }
                            }
                        }
                    }
                }
                /* (no post-processing of already occurred events is
                   needed anymore: the condition variable signal flag
                   machinery is gone) */

                /* local to global mapping */
                if (ev->ev_status != PTH_STATUS_PENDING)
                    any_occurred = TRUE;
            } while ((ev = ev->ev_next) != evh);
        }

        /* cancellation support */
        if (t->cancelreq == TRUE) {
            pth_debug2("pth_sched_eventmanager: cancellation request pending for thread \"%s\"", t->name);
            any_occurred = TRUE;
        }

        /* walk to next thread in waiting queue */
        tlast = t;
        t = pth_pqueue_walk(&g->WQ, t, PTH_WALK_NEXT);

        /*
         * move last thread to ready queue if any events occurred for it.
         * we insert it with a slightly increased queue priority to it a
         * better chance to immediately get scheduled, else the last running
         * thread might immediately get again the CPU which is usually not
         * what we want, because we oven use pth_yield() calls to give others
         * a chance.
         */
        if (any_occurred) {
            pth_pqueue_delete(&g->WQ, tlast);
            tlast->state = PTH_STATE_READY;
            pth_pqueue_insert(&g->RQ, tlast->prio+1, tlast);
            pth_debug2("pth_sched_eventmanager: thread \"%s\" moved from waiting "
                       "to ready queue", tlast->name);
        }
    }

    /* perhaps we have to internally loop... */
    if (loop_repeat) {
        pth_time_set(now, PTH_TIME_NOW);
        goto loop_entry;
    }

    pth_debug1("pth_sched_eventmanager: leaving");
    return;
}
#endif /* PTH_SCHED_EPOLL || PTH_SCHED_KQUEUE */


/* fd-wait backend dispatcher: poll(2) when built with -DPTH_SCHED_POLL,
   the classic select(2) path otherwise (both share all non-fd logic) */
intern void pth_sched_eventmanager(pth_time_t *now, int dopoll)
{
#if defined(PTH_SCHED_EPOLL) || defined(PTH_SCHED_KQUEUE)
    pth_sched_eventmanager_evport(now, dopoll);
#elif defined(PTH_SCHED_POLL)
    pth_sched_eventmanager_poll(now, dopoll);
#else
    pth_sched_eventmanager_select(now, dopoll);
#endif
}

intern void pth_sched_eventmanager_select(pth_time_t *now, int dopoll)
{
    pth_gsched_t *g = pth_gsched_active;
    pth_t nexttimer_thread;
    pth_event_t nexttimer_ev;
    pth_time_t nexttimer_value;
    pth_event_t evh;
    pth_event_t ev;
    pth_t t;
    pth_t tlast;
    int this_occurred;
    int any_occurred;
    fd_set rfds;
    fd_set wfds;
    fd_set efds;
    struct timeval delay;
    struct timeval *pdelay;
    sigset_t oss;
    struct sigaction sa;
    struct sigaction osa[1+PTH_NSIG];
    char minibuf[128];
    int loop_repeat;
    int fdmax;
    int rc;
    int sig;
    int n;

    pth_debug2("pth_sched_eventmanager: enter in %s mode",
               dopoll ? "polling" : "waiting");

    /* entry point for internal looping in event handling */
    loop_entry:
    loop_repeat = FALSE;

    /* clear the signal pipe and drain the wakeup inbox first: an already
       delivered wakeup must prevent us from blocking in select(2) below
       (producers push their message before writing the pipe byte) */
    while (pth_sc(read)(g->sigpipe[0], minibuf, sizeof(minibuf)) > 0) ;
    if (pth_gsched_drain(g))
        dopoll = TRUE;

    /* initialize fd sets */
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    fdmax = -1;

    /* initialize signal status */
    sigpending(&g->sigpending);
    sigfillset(&g->sigblock);
    sigemptyset(&g->sigcatch);
    sigemptyset(&g->sigraised);

    /* initialize next timer */
    pth_time_set(&nexttimer_value, PTH_TIME_ZERO);
    nexttimer_thread = NULL;
    nexttimer_ev = NULL;

    /* for all threads in the waiting queue... */
    any_occurred = FALSE;
    for (t = pth_pqueue_head(&g->WQ); t != NULL;
         t = pth_pqueue_walk(&g->WQ, t, PTH_WALK_NEXT)) {

        /* determine signals we block */
        for (sig = 1; sig < PTH_NSIG; sig++)
            if (!sigismember(&(t->mctx.sigs), sig))
                sigdelset(&g->sigblock, sig);

        /* cancellation support */
        if (t->cancelreq == TRUE)
            any_occurred = TRUE;

        /* ... and all their events... */
        if (t->events == NULL)
            continue;
        /* ...check whether events occurred */
        ev = evh = t->events;
        do {
            if (ev->ev_status == PTH_STATUS_PENDING) {
                this_occurred = FALSE;

                /* Filedescriptor I/O */
                if (ev->ev_type == PTH_EVENT_FD) {
                    /* filedescriptors are checked later all at once.
                       Here we only assemble them in the fd sets */
                    if (ev->ev_goal & PTH_UNTIL_FD_READABLE)
                        FD_SET(ev->ev_args.FD.fd, &rfds);
                    if (ev->ev_goal & PTH_UNTIL_FD_WRITEABLE)
                        FD_SET(ev->ev_args.FD.fd, &wfds);
                    if (ev->ev_goal & PTH_UNTIL_FD_EXCEPTION)
                        FD_SET(ev->ev_args.FD.fd, &efds);
                    if (fdmax < ev->ev_args.FD.fd)
                        fdmax = ev->ev_args.FD.fd;
                }
                /* Filedescriptor Set Select I/O */
                else if (ev->ev_type == PTH_EVENT_SELECT) {
                    /* filedescriptors are checked later all at once.
                       Here we only merge the fd sets. */
                    pth_util_fds_merge(ev->ev_args.SELECT.nfd,
                                       ev->ev_args.SELECT.rfds, &rfds,
                                       ev->ev_args.SELECT.wfds, &wfds,
                                       ev->ev_args.SELECT.efds, &efds);
                    if (fdmax < ev->ev_args.SELECT.nfd-1)
                        fdmax = ev->ev_args.SELECT.nfd-1;
                }
                /* Signal Set */
                else if (ev->ev_type == PTH_EVENT_SIGS) {
                    for (sig = 1; sig < PTH_NSIG; sig++) {
                        if (sigismember(ev->ev_args.SIGS.sigs, sig)) {
                            /* thread signal handling */
                            if (sigismember(&t->sigpending, sig)) {
                                *(ev->ev_args.SIGS.sig) = sig;
                                sigdelset(&t->sigpending, sig);
                                t->sigpendcnt--;
                                this_occurred = TRUE;
                            }
                            /* process signal handling */
                            if (sigismember(&g->sigpending, sig)) {
                                if (ev->ev_args.SIGS.sig != NULL)
                                    *(ev->ev_args.SIGS.sig) = sig;
                                pth_util_sigdelete(sig);
                                sigdelset(&g->sigpending, sig);
                                this_occurred = TRUE;
                            }
                            else {
                                sigdelset(&g->sigblock, sig);
                                sigaddset(&g->sigcatch, sig);
                            }
                        }
                    }
                }
                /* Timer */
                else if (ev->ev_type == PTH_EVENT_TIME) {
                    if (pth_time_cmp(&(ev->ev_args.TIME.tv), now) < 0)
                        this_occurred = TRUE;
                    else {
                        /* remember the timer which will be elapsed next */
                        if ((nexttimer_thread == NULL && nexttimer_ev == NULL) ||
                            pth_time_cmp(&(ev->ev_args.TIME.tv), &nexttimer_value) < 0) {
                            nexttimer_thread = t;
                            nexttimer_ev = ev;
                            pth_time_set(&nexttimer_value, &(ev->ev_args.TIME.tv));
                        }
                    }
                }
                /* Message Port Arrivals */
                else if (ev->ev_type == PTH_EVENT_MSG) {
                    /* mp_queue is shared with cross-scheduler pth_msgport_put;
                       read the predicate under the port's spinlock */
                    pth_spin_lock(&ev->ev_args.MSG.mp->mp_lock);
                    if (pth_ring_elements(&(ev->ev_args.MSG.mp->mp_queue)) > 0)
                        this_occurred = TRUE;
                    pth_spin_unlock(&ev->ev_args.MSG.mp->mp_lock);
                }
                /* Mutex Release and Condition Variable Signal events
                   are not polled anymore: threads park on the
                   primitive's wait queue and are woken explicitly
                   through the scheduler's wakeup inbox (pth_sync.c) */
                /* Thread Termination */
                else if (ev->ev_type == PTH_EVENT_TID) {
                    if (   (   ev->ev_args.TID.tid == NULL
                            && pth_pqueue_elements(&g->DQ) > 0)
                        || (   ev->ev_args.TID.tid != NULL
                            && ev->ev_args.TID.tid->state == ev->ev_goal))
                        this_occurred = TRUE;
                }
                /* Custom Event Function */
                else if (ev->ev_type == PTH_EVENT_FUNC) {
                    if (ev->ev_args.FUNC.func(ev->ev_args.FUNC.arg))
                        this_occurred = TRUE;
                    else {
                        pth_time_t tv;
                        pth_time_set(&tv, now);
                        pth_time_add(&tv, &(ev->ev_args.FUNC.tv));
                        if ((nexttimer_thread == NULL && nexttimer_ev == NULL) ||
                            pth_time_cmp(&tv, &nexttimer_value) < 0) {
                            nexttimer_thread = t;
                            nexttimer_ev = ev;
                            pth_time_set(&nexttimer_value, &tv);
                        }
                    }
                }

                /* tag event if it has occurred */
                if (this_occurred) {
                    pth_debug2("pth_sched_eventmanager: [non-I/O] event occurred for thread \"%s\"", t->name);
                    ev->ev_status = PTH_STATUS_OCCURRED;
                    any_occurred = TRUE;
                }
            }
        } while ((ev = ev->ev_next) != evh);
    }
    if (any_occurred)
        dopoll = TRUE;

    /* now decide how to poll for fd I/O and timers */
    if (dopoll) {
        /* do a polling with immediate timeout,
           i.e. check the fd sets only without blocking */
        pth_time_set(&delay, PTH_TIME_ZERO);
        pdelay = &delay;
    }
    else if (nexttimer_ev != NULL) {
        /* do a polling with a timeout set to the next timer,
           i.e. wait for the fd sets or the next timer */
        pth_time_set(&delay, &nexttimer_value);
        pth_time_sub(&delay, now);
        pdelay = &delay;
    }
    else {
        /* do a polling without a timeout,
           i.e. wait for the fd sets only with blocking */
        pdelay = NULL;
    }

    /* let select() wait for the read-part of the signal pipe
       (it was already cleared at the top of this event loop) */
    FD_SET(g->sigpipe[0], &rfds);
    if (fdmax < g->sigpipe[0])
        fdmax = g->sigpipe[0];

    /* replace signal actions for signals we've to catch for events
       (signal handling is the business of scheduler 0 only; auxiliary
       schedulers keep all signals permanently blocked) */
    if (g->id == 0) {
        for (sig = 1; sig < PTH_NSIG; sig++) {
            if (sigismember(&g->sigcatch, sig)) {
                sa.sa_handler = pth_sched_eventmanager_sighandler;
                sigfillset(&sa.sa_mask);
                sa.sa_flags = 0;
                sigaction(sig, &sa, &osa[sig]);
            }
        }

        /* allow some signals to be delivered: Either to our
           catching handler or directly to the configured
           handler for signals not catched by events */
        pth_sc(sigprocmask)(SIG_SETMASK, &g->sigblock, &oss);
    }

    /* now do the polling for filedescriptor I/O and timers
       WHEN THE SCHEDULER SLEEPS AT ALL, THEN HERE!! */
    rc = -1;
    if (!(dopoll && fdmax == -1))
        while ((rc = pth_sc(select)(fdmax+1, &rfds, &wfds, &efds, pdelay)) < 0
               && errno == EINTR) ;

    /* restore signal mask and actions and handle signals */
    if (g->id == 0) {
        pth_sc(sigprocmask)(SIG_SETMASK, &oss, NULL);
        for (sig = 1; sig < PTH_NSIG; sig++)
            if (sigismember(&g->sigcatch, sig))
                sigaction(sig, &osa[sig], NULL);
    }

    /* drain the wakeup inbox again: messages which arrived while we
       were sleeping in select(2) must be applied before the cleanup
       loop below moves threads out of the waiting queue */
    pth_gsched_drain(g);

    /* if the timer elapsed, handle it */
    if (!dopoll && rc == 0 && nexttimer_ev != NULL) {
        if (nexttimer_ev->ev_type == PTH_EVENT_FUNC) {
            /* it was an implicit timer event for a function event,
               so repeat the event handling for rechecking the function */
            loop_repeat = TRUE;
        }
        else {
            /* it was an explicit timer event, standing for its own */
            pth_debug2("pth_sched_eventmanager: [timeout] event occurred for thread \"%s\"",
                       nexttimer_thread->name);
            nexttimer_ev->ev_status = PTH_STATUS_OCCURRED;
        }
    }

    /* if the internal signal pipe was used, adjust the select() results */
    if (!dopoll && rc > 0 && FD_ISSET(g->sigpipe[0], &rfds)) {
        FD_CLR(g->sigpipe[0], &rfds);
        rc--;
    }

    /* if an error occurred, avoid confusion in the cleanup loop */
    if (rc <= 0) {
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
    }

    /* now comes the final cleanup loop where we've to
       do two jobs: first we've to do the late handling of the fd I/O events and
       additionally if a thread has one occurred event, we move it from the
       waiting queue to the ready queue */

    /* for all threads in the waiting queue... */
    t = pth_pqueue_head(&g->WQ);
    while (t != NULL) {

        /* do the late handling of the fd I/O and signal
           events in the waiting event ring */
        any_occurred = FALSE;
        if (t->events != NULL) {
            ev = evh = t->events;
            do {
                /*
                 * Late handling for still not occured events
                 */
                if (ev->ev_status == PTH_STATUS_PENDING) {
                    /* Filedescriptor I/O */
                    if (ev->ev_type == PTH_EVENT_FD) {
                        if (   (   ev->ev_goal & PTH_UNTIL_FD_READABLE
                                && FD_ISSET(ev->ev_args.FD.fd, &rfds))
                            || (   ev->ev_goal & PTH_UNTIL_FD_WRITEABLE
                                && FD_ISSET(ev->ev_args.FD.fd, &wfds))
                            || (   ev->ev_goal & PTH_UNTIL_FD_EXCEPTION
                                && FD_ISSET(ev->ev_args.FD.fd, &efds)) ) {
                            pth_debug2("pth_sched_eventmanager: "
                                       "[I/O] event occurred for thread \"%s\"", t->name);
                            ev->ev_status = PTH_STATUS_OCCURRED;
                        }
                        else if (rc < 0) {
                            /* re-check particular filedescriptor */
                            int rc2;
                            if (ev->ev_goal & PTH_UNTIL_FD_READABLE)
                                FD_SET(ev->ev_args.FD.fd, &rfds);
                            if (ev->ev_goal & PTH_UNTIL_FD_WRITEABLE)
                                FD_SET(ev->ev_args.FD.fd, &wfds);
                            if (ev->ev_goal & PTH_UNTIL_FD_EXCEPTION)
                                FD_SET(ev->ev_args.FD.fd, &efds);
                            pth_time_set(&delay, PTH_TIME_ZERO);
                            while ((rc2 = pth_sc(select)(ev->ev_args.FD.fd+1, &rfds, &wfds, &efds, &delay)) < 0
                                   && errno == EINTR) ;
                            if (rc2 > 0) {
                                /* cleanup afterwards for next iteration */
                                FD_CLR(ev->ev_args.FD.fd, &rfds);
                                FD_CLR(ev->ev_args.FD.fd, &wfds);
                                FD_CLR(ev->ev_args.FD.fd, &efds);
                            } else if (rc2 < 0) {
                                /* cleanup afterwards for next iteration */
                                FD_ZERO(&rfds);
                                FD_ZERO(&wfds);
                                FD_ZERO(&efds);
                                ev->ev_status = PTH_STATUS_FAILED;
                                pth_debug2("pth_sched_eventmanager: "
                                           "[I/O] event failed for thread \"%s\"", t->name);
                            }
                        }
                    }
                    /* Filedescriptor Set I/O */
                    else if (ev->ev_type == PTH_EVENT_SELECT) {
                        if (pth_util_fds_test(ev->ev_args.SELECT.nfd,
                                              ev->ev_args.SELECT.rfds, &rfds,
                                              ev->ev_args.SELECT.wfds, &wfds,
                                              ev->ev_args.SELECT.efds, &efds)) {
                            n = pth_util_fds_select(ev->ev_args.SELECT.nfd,
                                                    ev->ev_args.SELECT.rfds, &rfds,
                                                    ev->ev_args.SELECT.wfds, &wfds,
                                                    ev->ev_args.SELECT.efds, &efds);
                            if (ev->ev_args.SELECT.n != NULL)
                                *(ev->ev_args.SELECT.n) = n;
                            ev->ev_status = PTH_STATUS_OCCURRED;
                            pth_debug2("pth_sched_eventmanager: "
                                       "[I/O] event occurred for thread \"%s\"", t->name);
                        }
                        else if (rc < 0) {
                            /* re-check particular filedescriptor set */
                            int rc2;
                            fd_set *prfds = NULL;
                            fd_set *pwfds = NULL;
                            fd_set *pefds = NULL;
                            fd_set trfds;
                            fd_set twfds;
                            fd_set tefds;
                            if (ev->ev_args.SELECT.rfds) {
                                memcpy(&trfds, ev->ev_args.SELECT.rfds, sizeof(rfds));
                                prfds = &trfds;
                            }
                            if (ev->ev_args.SELECT.wfds) {
                                memcpy(&twfds, ev->ev_args.SELECT.wfds, sizeof(wfds));
                                pwfds = &twfds;
                            }
                            if (ev->ev_args.SELECT.efds) {
                                memcpy(&tefds, ev->ev_args.SELECT.efds, sizeof(efds));
                                pefds = &tefds;
                            }
                            pth_time_set(&delay, PTH_TIME_ZERO);
                            while ((rc2 = pth_sc(select)(ev->ev_args.SELECT.nfd+1, prfds, pwfds, pefds, &delay)) < 0
                                   && errno == EINTR) ;
                            if (rc2 < 0) {
                                ev->ev_status = PTH_STATUS_FAILED;
                                pth_debug2("pth_sched_eventmanager: "
                                           "[I/O] event failed for thread \"%s\"", t->name);
                            }
                        }
                    }
                    /* Signal Set */
                    else if (ev->ev_type == PTH_EVENT_SIGS) {
                        for (sig = 1; sig < PTH_NSIG; sig++) {
                            if (sigismember(ev->ev_args.SIGS.sigs, sig)) {
                                if (sigismember(&g->sigraised, sig)) {
                                    if (ev->ev_args.SIGS.sig != NULL)
                                        *(ev->ev_args.SIGS.sig) = sig;
                                    pth_debug2("pth_sched_eventmanager: "
                                               "[signal] event occurred for thread \"%s\"", t->name);
                                    sigdelset(&g->sigraised, sig);
                                    ev->ev_status = PTH_STATUS_OCCURRED;
                                }
                            }
                        }
                    }
                }
                /* (no post-processing of already occurred events is
                   needed anymore: the condition variable signal flag
                   machinery is gone) */

                /* local to global mapping */
                if (ev->ev_status != PTH_STATUS_PENDING)
                    any_occurred = TRUE;
            } while ((ev = ev->ev_next) != evh);
        }

        /* cancellation support */
        if (t->cancelreq == TRUE) {
            pth_debug2("pth_sched_eventmanager: cancellation request pending for thread \"%s\"", t->name);
            any_occurred = TRUE;
        }

        /* walk to next thread in waiting queue */
        tlast = t;
        t = pth_pqueue_walk(&g->WQ, t, PTH_WALK_NEXT);

        /*
         * move last thread to ready queue if any events occurred for it.
         * we insert it with a slightly increased queue priority to it a
         * better chance to immediately get scheduled, else the last running
         * thread might immediately get again the CPU which is usually not
         * what we want, because we oven use pth_yield() calls to give others
         * a chance.
         */
        if (any_occurred) {
            pth_pqueue_delete(&g->WQ, tlast);
            tlast->state = PTH_STATE_READY;
            pth_pqueue_insert(&g->RQ, tlast->prio+1, tlast);
            pth_debug2("pth_sched_eventmanager: thread \"%s\" moved from waiting "
                       "to ready queue", tlast->name);
        }
    }

    /* perhaps we have to internally loop... */
    if (loop_repeat) {
        pth_time_set(now, PTH_TIME_NOW);
        goto loop_entry;
    }

    pth_debug1("pth_sched_eventmanager: leaving");
    return;
}

intern void pth_sched_eventmanager_sighandler(int sig)
{
    pth_gsched_t *g = pth_gsched_active;
    char c;

    /* remember raised signal */
    sigaddset(&g->sigraised, sig);

    /* write signal to signal pipe in order to awake the select() */
    c = (int)sig;
    pth_sc(write)(g->sigpipe[1], &c, sizeof(char));
    return;
}



/*
** ____ MULTI-SCHEDULER RUNTIME (MP) _________________________________
*/

#ifdef PTH_MP
/* bootstrap function running on each auxiliary scheduler's OS thread */
static void *pth_gsched_bootstrap(void *arg)
{
    pth_gsched_t *g = (pth_gsched_t *)arg;
    pth_attr_t t_attr;
    sigset_t ss;

    /* route all Pth operations on this OS thread to our scheduler */
    pth_gsched_active = g;

    /* signals are the business of scheduler 0 only */
    sigfillset(&ss);
    PTH_OS_SIGMASK(SIG_SETMASK, &ss, NULL);

    /* per-scheduler state (queues, signal pipe, inbox) */
    if (!pth_scheduler_init()) {
        g->boot_rc = FALSE;
        pth_atomic_store(&g->boot_done, 1);
        return NULL;
    }

    /* spawn the scheduler thread (green stack) */
    t_attr = pth_attr_new();
    pth_attr_set(t_attr, PTH_ATTR_PRIO,         PTH_PRIO_MAX);
    pth_attr_set(t_attr, PTH_ATTR_NAME,         "**SCHEDULER**");
    pth_attr_set(t_attr, PTH_ATTR_JOINABLE,     FALSE);
    pth_attr_set(t_attr, PTH_ATTR_CANCEL_STATE, PTH_CANCEL_DISABLE);
    pth_attr_set(t_attr, PTH_ATTR_STACK_SIZE,   64*1024);
    pth_attr_set(t_attr, PTH_ATTR_STACK_ADDR,   NULL);
    g->sched = pth_spawn(t_attr, pth_scheduler, NULL);
    pth_attr_destroy(t_attr);
    if (g->sched == NULL) {
        pth_scheduler_kill();
        g->boot_rc = FALSE;
        pth_atomic_store(&g->boot_done, 1);
        return NULL;
    }

    /* report success and enter the scheduler; it returns control here
       only after a PTH_GMSG_STOP once it has fully run dry */
    g->boot_rc = TRUE;
    pth_atomic_store(&g->boot_done, 1);
    g->current = g->sched;
    pth_mctx_switch(&g->bootmctx, &g->sched->mctx);

    /* shutdown: release the scheduler's resources */
    pth_tcb_free(g->sched);
    g->sched = NULL;
    pth_scheduler_kill();
    pth_atomic_inc(&pth_gsched_nexited);
    return NULL;
}
#endif /* PTH_MP */

/* start additional schedulers until `nsched' of them exist */
int pth_mp_init(int nsched)
{
    if (nsched < 1 || nsched > PTH_GSCHED_MAX)
        return pth_error(FALSE, EINVAL);

    /* implicit initialization of the primary scheduler */
    pth_implicit_init();

    /* may only be used from the primary scheduler */
    if (pth_gsched_active != &pth_gsched_pri)
        return pth_error(FALSE, EPERM);

#ifndef PTH_MP
    if (nsched != 1)
        return pth_error(FALSE, ENOSYS);
    return TRUE;
#else
    while (pth_atomic_load(&pth_gsched_ntab) < nsched) {
        pth_gsched_t *g;
        int id = pth_atomic_load(&pth_gsched_ntab);

        if ((g = (pth_gsched_t *)calloc(1, sizeof(pth_gsched_t))) == NULL)
            return pth_error(FALSE, ENOMEM);
        g->id = id;
        pth_atomic_store(&g->boot_done, 0);
        if (PTH_OS_CREATE(&g->osthread, pth_gsched_bootstrap, g) != 0) {
            free(g);
            return pth_error(FALSE, EAGAIN);
        }
        while (!pth_atomic_load(&g->boot_done))
            pth_nap(pth_time(0, 1000));
        if (!g->boot_rc) {
            free(g);
            return pth_error(FALSE, EAGAIN);
        }
        pth_gsched_tab[id] = g;
        pth_atomic_inc(&pth_gsched_ntab);
    }
    return TRUE;
#endif
}

/* stop all auxiliary schedulers (blocks until each has run dry; make sure
   their threads have terminated, or arrange for them to terminate, first) */
int pth_mp_shutdown(void)
{
#ifdef PTH_MP
    int i, n;

    if (!pth_initialized)
        return pth_error(FALSE, EINVAL);
    if (pth_gsched_active != &pth_gsched_pri)
        return pth_error(FALSE, EPERM);
    n = pth_atomic_load(&pth_gsched_ntab);
    for (i = 1; i < n; i++)
        pth_gsched_post(pth_gsched_tab[i], PTH_GMSG_STOP, NULL, NULL);
    /* IMPORTANT: do not block this OS thread in pthread_join(3) while the
       auxiliaries are still winding down: our own scheduler must keep
       running, because threads on it may be part of the wakeup chains
       (e.g. barrier mutex handoffs) the auxiliaries' threads are waiting
       on. So nap cooperatively until every auxiliary has announced its
       exit, and only then reap the (already finished) OS threads. */
    while (pth_atomic_load(&pth_gsched_nexited) < n - 1)
        pth_nap(pth_time(0, 1000));
    for (i = 1; i < n; i++) {
        PTH_OS_JOIN(pth_gsched_tab[i]->osthread);
        free(pth_gsched_tab[i]);
        pth_gsched_tab[i] = NULL;
    }
    pth_atomic_store(&pth_gsched_ntab, 1);
    pth_atomic_store(&pth_gsched_nexited, 0);
#endif
    return TRUE;
}

/* number of running schedulers */
int pth_sched_count(void)
{
    return pth_atomic_load(&pth_gsched_ntab);
}

/* scheduler identifier of the calling thread */
int pth_sched_id(void)
{
    return pth_gsched_active->id;
}
