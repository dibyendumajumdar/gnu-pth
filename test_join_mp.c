/* Cross-scheduler pth_join (phase 4): a thread on scheduler 0 joins threads
   living on other schedulers, receiving their return values, with no hang
   and no use-after-free. Also checks the already-dead fast path and that
   the same-scheduler path still works. */
#include "pth.h"
#include <stdio.h>
#include <stdlib.h>

#define NSCHED 4
#define NJOBS  200

static void *retworker(void *arg)
{
    long id = (long)arg;
    int i;
    for (i = 0; i < 3; i++)
        pth_yield(NULL);              /* encourage the joiner to block */
    return (void *)(0x1000 + id);
}

static void *quickworker(void *arg)
{
    return (void *)0xBEEF;            /* returns immediately */
}

/* a joiner thread (lives on scheduler 0) that itself spawns a worker on a
   remote scheduler and joins it, verifying the returned value */
static long cj_bad;
#define CJ_JOINERS 8
#define CJ_ITER    40
static void *cjoiner(void *arg)
{
    long base = (long)arg;
    int i;
    for (i = 0; i < CJ_ITER; i++) {
        pth_attr_t a = pth_attr_new();
        long tag = base*10000 + i;
        pth_t w;
        void *r = NULL;
        pth_attr_set(a, PTH_ATTR_JOINABLE, TRUE);
        w = pth_spawn_on(1 + (int)(tag % (NSCHED-1)), a, retworker, (void *)tag);
        if (!(w && pth_join(w, &r) == TRUE && r == (void *)(0x1000 + tag)))
            cj_bad++;
        pth_attr_destroy(a);
    }
    return NULL;
}

int main(void)
{
    pth_attr_t attr;
    pth_t t;
    void *ret;
    int i, fails = 0;
    long bad = 0;

    pth_init();
    if (!pth_mp_init(NSCHED)) { fprintf(stderr, "built without PTH_MP\n"); return 1; }
    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);

    /* 1: join one remote thread and read its value (joiner blocks) */
    t = pth_spawn_on(1, attr, retworker, (void *)7);
    ret = NULL;
    if (!(t && pth_join(t, &ret) == TRUE && ret == (void *)0x1007)) fails++;
    printf("1 remote join + value : %s\n", ret == (void *)0x1007 ? "OK" : "FAIL");

    /* 2: remote thread that has already finished (join_done fast path) */
    t = pth_spawn_on(2, attr, quickworker, NULL);
    for (i = 0; i < 30; i++) pth_yield(NULL);   /* give it time to finish */
    ret = NULL;
    if (!(t && pth_join(t, &ret) == TRUE && ret == (void *)0xBEEF)) fails++;
    printf("2 already-dead path   : %s\n", ret == (void *)0xBEEF ? "OK" : "FAIL");

    /* 3: many cross-scheduler joins spread over schedulers 1..NSCHED-1 */
    for (i = 0; i < NJOBS; i++) {
        int s = 1 + (i % (NSCHED - 1));
        t = pth_spawn_on(s, attr, retworker, (void *)(long)i);
        ret = NULL;
        if (!(t && pth_join(t, &ret) == TRUE && ret == (void *)(long)(0x1000 + i)))
            bad++;
    }
    printf("3 %d cross-sched joins: %s\n", NJOBS, bad == 0 ? "OK" : "FAIL");
    if (bad) fails++;

    /* 4: same-scheduler join still works (regression) */
    t = pth_spawn_on(0, attr, retworker, (void *)42);
    ret = NULL;
    if (!(t && pth_join(t, &ret) == TRUE && ret == (void *)0x102a)) fails++;
    printf("4 same-sched join     : %s\n", ret == (void *)0x102a ? "OK" : "FAIL");

    /* 5: many concurrent joiner threads (on scheduler 0), each spawning and
          joining remote workers -- stresses cross-scheduler wakeups + reaps
          for lost-wakeup hangs */
    {
        pth_t j[CJ_JOINERS];
        int k;
        cj_bad = 0;
        for (k = 0; k < CJ_JOINERS; k++)
            j[k] = pth_spawn(attr, cjoiner, (void *)(long)k);
        for (k = 0; k < CJ_JOINERS; k++)
            pth_join(j[k], NULL);      /* same-scheduler join of the joiners */
        printf("5 %d concurrent joiners x %d: %s\n",
               CJ_JOINERS, CJ_ITER, cj_bad == 0 ? "OK" : "FAIL");
        if (cj_bad) fails++;
    }

    pth_attr_destroy(attr);
    pth_mp_shutdown();
    printf(fails == 0 ? "ALL JOIN-MP TESTS PASSED\n" : "%d JOIN-MP TESTS FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
