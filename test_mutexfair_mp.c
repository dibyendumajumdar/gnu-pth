/* Cross-scheduler correctness of a FAIR mutex: many threads spread over
   several schedulers hammer one fair mutex. Verifies handoff delivers
   wakeups across schedulers with no lost-wakeup hang and exact mutual
   exclusion.

   Completion is awaited with a cross-scheduler barrier (built on the
   cross-scheduler mutex+cond primitives), NOT pth_join -- pth_join does
   not cross schedulers in this build. */
#include "pth.h"
#include <stdio.h>
#include <stdlib.h>

#define NSCHED 4
#define NPER   4
#define ITER   2000

static pth_mutex_t   mtx;
static pth_barrier_t donebar;
static long counter;

static void *hammer(void *arg)
{
    int i;
    for (i = 0; i < ITER; i++) {
        pth_mutex_acquire(&mtx, FALSE, NULL);
        counter++;
        pth_mutex_release(&mtx);
    }
    pth_barrier_reach(&donebar);
    return NULL;
}

int main(void)
{
    pth_attr_t attr;
    int s, k;

    if (!pth_mp_init(NSCHED)) {
        fprintf(stderr, "pth_mp_init failed (built without PTH_MP?)\n");
        return 1;
    }
    pth_mutex_init(&mtx);
    pth_mutex_setfair(&mtx, TRUE);
    pth_barrier_init(&donebar, NSCHED*NPER + 1);   /* workers + main */
    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);

    for (s = 0; s < NSCHED; s++)
        for (k = 0; k < NPER; k++)
            if (pth_spawn_on(s, attr, hammer, NULL) == NULL) {
                fprintf(stderr, "pth_spawn_on(%d) failed\n", s);
                return 1;
            }

    pth_barrier_reach(&donebar);   /* wait for all workers cross-scheduler */

    pth_attr_destroy(attr);
    pth_mp_shutdown();

    printf("fair-mp: counter=%ld (want %d) %s\n",
           counter, NSCHED*NPER*ITER,
           counter == (long)NSCHED*NPER*ITER ? "OK" : "FAIL");
    return counter == (long)NSCHED*NPER*ITER ? 0 : 1;
}
