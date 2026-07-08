/* 2nd-review finding: if the once initializer is cancelled, the control must
   not stay stuck IN_PROGRESS -- a later caller must be able to re-run it. */
#include "pth.h"
#include <stdio.h>
#include <stdlib.h>

static pth_once_t once = PTH_ONCE_INIT;
static volatile int started = 0;      /* how many times the initializer began */
static volatile int completed = 0;    /* set when it runs to completion        */
static volatile int first = 1;        /* first invocation blocks (to be cancelled) */

static void initializer(void *arg)
{
    __atomic_add_fetch(&started, 1, __ATOMIC_SEQ_CST);
    if (__atomic_exchange_n(&first, 0, __ATOMIC_SEQ_CST)) {
        /* first caller: block at a cancellation point until cancelled */
        for (;;) {
            pth_cancel_point();
            pth_nap(pth_time(0, 1000));
        }
    }
    /* a later caller: complete normally */
    __atomic_store_n(&completed, 1, __ATOMIC_SEQ_CST);
}

static void *caller(void *arg) { pth_once(&once, initializer, NULL); return (void *)0x7; }

int main(void)
{
    pth_attr_t attr;
    pth_t a, b;
    void *ra = NULL, *rb = NULL;
    int fails = 0;

    pth_init();
    if (!pth_mp_init(4)) { fprintf(stderr, "no PTH_MP\n"); return 1; }
    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);

    a = pth_spawn_on(1, attr, caller, NULL);          /* wins, blocks in init */
    pth_nap(pth_time(0, 20000));
    b = pth_spawn_on(2, attr, caller, NULL);          /* waits for state DONE  */
    pth_nap(pth_time(0, 20000));

    pth_cancel(a);                                    /* cancel the initializer */
    /* b must now be able to re-run the initializer to completion */
    rb = NULL;
    pth_join(b, &rb);                                 /* hangs if control stuck at 1 */
    ra = NULL;
    pth_join(a, &ra);

    printf("initializer started=%d completed=%d\n", started, completed);
    printf("A (cancelled) join=%s ; B (retried) join=%s\n",
           ra==PTH_CANCELED?"CANCELED":"?", rb==(void*)0x7?"returned":"?");
    if (completed != 1) { printf("FAIL: initializer never completed\n"); fails++; }
    if (started != 2)   { printf("FAIL: expected 2 starts (aborted + retried), got %d\n", started); fails++; }
    if (rb != (void*)0x7) { printf("FAIL: B did not return from pth_once\n"); fails++; }

    /* a fresh caller must see it done and not re-run */
    { pth_t c = pth_spawn_on(3, attr, caller, NULL); void *rc=NULL; pth_join(c,&rc); }
    if (started != 2) { printf("FAIL: initializer re-ran after completion (started=%d)\n", started); fails++; }

    pth_attr_destroy(attr);
    pth_mp_shutdown();
    printf(fails==0 ? "ALL ONCE-CANCEL TESTS PASSED\n" : "%d ONCE-CANCEL TESTS FAILED\n", fails);
    return fails==0 ? 0 : 1;
}
