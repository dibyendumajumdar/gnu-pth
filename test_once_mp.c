/* REVIEW.md follow-up finding: pth_once must run the constructor exactly once
   even when many threads across several schedulers race on the same control. */
#include "pth.h"
#include <stdio.h>
#include <stdlib.h>

#define NSCHED 4
#define NCALLERS 40

static pth_once_t once = PTH_ONCE_INIT;
static volatile int ctor_runs = 0;
static pth_barrier_t startbar;   /* release all callers at once for contention */

static void constructor(void *arg)
{
    __atomic_fetch_add(&ctor_runs, 1, __ATOMIC_SEQ_CST);  /* count runs */
    pth_nap(pth_time(0, 5000));        /* widen the window so racing schedulers overlap */
}

static void *caller(void *arg)
{
    pth_barrier_reach(&startbar);
    pth_once(&once, constructor, NULL);
    pth_barrier_reach(&startbar);
    return NULL;
}

int main(void)
{
    pth_attr_t attr;
    int s, k, n = 0;

    pth_init();
    if (!pth_mp_init(NSCHED)) { fprintf(stderr, "no PTH_MP\n"); return 1; }
    pth_barrier_init(&startbar, NCALLERS + 1);
    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, FALSE);

    for (s = 0; s < NSCHED; s++)
        for (k = 0; k < NCALLERS/NSCHED; k++, n++)
            pth_spawn_on(s, attr, caller, NULL);

    pth_barrier_reach(&startbar);   /* GO */
    pth_barrier_reach(&startbar);   /* all callers returned from pth_once */

    pth_attr_destroy(attr);
    pth_mp_shutdown();

    {
        int runs = __atomic_load_n(&ctor_runs, __ATOMIC_SEQ_CST);
        printf("callers=%d constructor_runs=%d %s\n", n, runs, runs==1?"OK":"FAIL");
        printf(runs==1 ? "ALL ONCE-MP TESTS PASSED\n" : "ONCE-MP TESTS FAILED\n");
        return runs==1 ? 0 : 1;
    }
}
