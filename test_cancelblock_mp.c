/* Regression for REVIEW.md finding 1 (async cancel of a thread blocked on /
   holding a cross-scheduler mutex) and finding 2 (cross-scheduler pth_abort).
   Before the fixes, async-cancelling a mutex-blocked thread left a stale/freed
   TCB in the mutex wait queue -> use-after-free on the next release; and a
   cancelled thread holding a mutex failed to release it. */
#include "pth.h"
#include <stdio.h>
#include <stdlib.h>

static pth_mutex_t mA;      /* main holds it; worker blocks on it       */
static pth_mutex_t mB;      /* worker holds it, then is cancelled        */
static volatile int stop = 0;

/* blocks trying to acquire mA (held by main), with async cancel enabled */
static void *blocker(void *arg)
{
    pth_cancel_state(PTH_CANCEL_ENABLE|PTH_CANCEL_ASYNCHRONOUS, NULL);
    pth_mutex_acquire(&mA, FALSE, NULL);   /* parks on mA's wait queue */
    pth_mutex_release(&mA);
    return (void *)0x1;
}

/* acquires mB then blocks napping, with async cancel enabled */
static void *holder(void *arg)
{
    pth_mutex_acquire(&mB, FALSE, NULL);
    pth_cancel_state(PTH_CANCEL_ENABLE|PTH_CANCEL_ASYNCHRONOUS, NULL);
    while (!stop)
        pth_nap(pth_time(10, 0));
    return (void *)0x2;
}

/* like blocker() but DETACHED: async cancel frees its TCB immediately, so a
   stale mutex-wait-queue entry becomes a use-after-free on the next release */
static void *blocker_detached(void *arg)
{
    pth_cancel_state(PTH_CANCEL_ENABLE|PTH_CANCEL_ASYNCHRONOUS, NULL);
    pth_mutex_acquire(&mA, FALSE, NULL);
    pth_mutex_release(&mA);
    return NULL;
}

/* spins forever (until aborted) -- target for pth_abort */
static void *runaway(void *arg)
{
    for (;;) {
        pth_cancel_point();
        pth_nap(pth_time(0, 1000));
    }
    return (void *)0x3;
}

int main(void)
{
    pth_attr_t attr;
    pth_t w;
    void *ret;
    int fails = 0;

    pth_init();
    if (!pth_mp_init(4)) { fprintf(stderr, "no PTH_MP\n"); return 1; }
    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);

    /* A: async-cancel a thread blocked on a mutex, then release the mutex */
    pth_mutex_init(&mA);
    pth_mutex_acquire(&mA, FALSE, NULL);               /* main owns mA */
    w = pth_spawn_on(1, attr, blocker, NULL);
    pth_nap(pth_time(0, 20000));                        /* let it park on mA */
    pth_cancel(w);
    pth_nap(pth_time(0, 30000));                        /* let sched 1 apply it */
    pth_mutex_release(&mA);                             /* must NOT crash */
    ret = NULL; pth_join(w, &ret);
    printf("A blocked-on-mutex async cancel: ret=%s %s\n",
           ret==PTH_CANCELED?"CANCELED":"?", ret==PTH_CANCELED?"OK":"FAIL");
    if (ret != PTH_CANCELED) fails++;
    /* mA must be usable again (wait queue not corrupted) */
    if (pth_mutex_acquire(&mA, TRUE, NULL) != TRUE) { printf("A2 FAIL: mA unusable\n"); fails++; }
    else { pth_mutex_release(&mA); printf("A2 mutex still usable: OK\n"); }

    /* D: DETACHED thread blocked on mA, async-cancelled (TCB freed), then mA
          released -- exercises the use-after-free on a stale wait-queue entry */
    {
        pth_attr_t dattr = pth_attr_new();
        pth_t d;
        pth_attr_set(dattr, PTH_ATTR_JOINABLE, FALSE);
        pth_mutex_acquire(&mA, FALSE, NULL);           /* main owns mA again */
        d = pth_spawn_on(1, dattr, blocker_detached, NULL);
        pth_nap(pth_time(0, 20000));                    /* let it park on mA */
        pth_cancel(d);                                  /* async: frees the TCB */
        pth_nap(pth_time(0, 30000));                    /* let sched 1 apply it */
        pth_mutex_release(&mA);                         /* UAF here without fix */
        pth_nap(pth_time(0, 20000));
        if (pth_mutex_acquire(&mA, TRUE, NULL) != TRUE) { printf("D FAIL: mA unusable after detached cancel\n"); fails++; }
        else { pth_mutex_release(&mA); printf("D detached blocked cancel (no UAF): OK\n"); }
        pth_attr_destroy(dattr);
    }

    /* B: async-cancel a thread that HOLDS a mutex -> it must be released */
    pth_mutex_init(&mB);
    w = pth_spawn_on(2, attr, holder, NULL);
    pth_nap(pth_time(0, 20000));                        /* let it acquire mB */
    pth_cancel(w);
    pth_nap(pth_time(0, 30000));
    ret = NULL; pth_join(w, &ret);
    if (pth_mutex_acquire(&mB, TRUE, NULL) != TRUE) { printf("B FAIL: mB not released by cancelled holder\n"); fails++; }
    else { pth_mutex_release(&mB); printf("B cancelled holder released mutex: OK\n"); }

    /* C: cross-scheduler pth_abort of a running (detached-by-abort) thread.
          A clean shutdown proves the abort took effect (its scheduler ran dry). */
    w = pth_spawn_on(3, attr, runaway, NULL);
    pth_nap(pth_time(0, 20000));
    if (pth_abort(w) != TRUE) { printf("C FAIL: pth_abort returned FALSE\n"); fails++; }
    else printf("C pth_abort(foreign) accepted: OK\n");

    stop = 1;
    pth_attr_destroy(attr);
    pth_mp_shutdown();          /* hangs if C's runaway was not aborted */

    printf(fails==0 ? "ALL CANCELBLOCK-MP TESTS PASSED\n" : "%d CANCELBLOCK-MP TESTS FAILED\n", fails);
    return fails==0 ? 0 : 1;
}
