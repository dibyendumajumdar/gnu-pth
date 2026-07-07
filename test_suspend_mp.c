/* Cross-scheduler pth_suspend / pth_resume / pth_yield(to): the main thread on
   scheduler 0 suspends a worker running on scheduler 1, verifies it stops
   making progress, resumes it, and verifies it runs again. */
#include "pth.h"
#include <stdio.h>
#include <stdlib.h>

static volatile long counter = 0;
static volatile int  stop = 0;

static void *worker(void *arg)
{
    while (!stop) {
        counter++;
        pth_nap(pth_time(0, 1000));   /* 1ms: lets its scheduler go idle+drain */
    }
    return (void *)0xABCD;
}

int main(void)
{
    pth_attr_t attr;
    pth_t w;
    void *ret = NULL;
    long c1, c2, c3, c4;
    int fails = 0;

    pth_init();
    if (!pth_mp_init(4)) { fprintf(stderr, "no PTH_MP\n"); return 1; }
    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);

    w = pth_spawn_on(1, attr, worker, NULL);

    /* cross-scheduler yield-to hint must not error */
    pth_nap(pth_time(0, 20000));
    if (pth_yield(w) != TRUE) { printf("yield(to) cross-sched returned FALSE\n"); fails++; }

    pth_nap(pth_time(0, 20000)); c1 = counter;           /* running */
    if (pth_suspend(w) != TRUE) { printf("pth_suspend returned FALSE\n"); fails++; }
    pth_nap(pth_time(0, 30000)); c2 = counter;           /* suspend settled */
    pth_nap(pth_time(0, 30000)); c3 = counter;           /* still suspended */

    printf("running delta (pre-suspend): c1=%ld\n", c1);
    printf("suspended: c2=%ld c3=%ld (frozen? %s)\n", c2, c3, c3==c2 ? "yes":"NO");
    if (c3 != c2) { printf("FAIL: counter advanced while suspended (%ld -> %ld)\n", c2, c3); fails++; }

    if (pth_resume(w) != TRUE) { printf("pth_resume returned FALSE\n"); fails++; }
    pth_nap(pth_time(0, 30000)); c4 = counter;           /* progressing again */
    printf("resumed: c4=%ld (progress? %s)\n", c4, c4>c3 ? "yes":"NO");
    if (c4 <= c3) { printf("FAIL: counter did not advance after resume\n"); fails++; }

    stop = 1;
    pth_join(w, &ret);
    if (ret != (void *)0xABCD) { printf("FAIL: bad join value %p\n", ret); fails++; }

    pth_attr_destroy(attr);
    pth_mp_shutdown();
    printf(fails==0 ? "ALL SUSPEND/RESUME-MP TESTS PASSED\n" : "%d SUSPEND/RESUME-MP TESTS FAILED\n", fails);
    return fails==0 ? 0 : 1;
}
