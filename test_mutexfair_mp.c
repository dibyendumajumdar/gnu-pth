/* Cross-scheduler correctness of a FAIR mutex: many threads spread over
   several schedulers hammer one fair mutex. Verifies handoff delivers
   wakeups across schedulers with no lost-wakeup hang and exact mutual
   exclusion. */
#include "pth.h"
#include <stdio.h>
#include <stdlib.h>

#define NSCHED 4
#define NPER   4
#define ITER   2000

static pth_mutex_t mtx;
static long counter;

static void *hammer(void *arg)
{
    int i;
    for (i = 0; i < ITER; i++) {
        pth_mutex_acquire(&mtx, FALSE, NULL);
        counter++;
        pth_mutex_release(&mtx);
    }
    return NULL;
}

int main(void)
{
    pth_t th[NSCHED*NPER];
    pth_attr_t attr;
    int s, k, n = 0;

    if (!pth_mp_init(NSCHED)) {
        fprintf(stderr, "pth_mp_init failed (built without PTH_MP?)\n");
        return 1;
    }
    pth_mutex_init(&mtx);
    pth_mutex_setfair(&mtx, TRUE);
    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);

    for (s = 0; s < NSCHED; s++)
        for (k = 0; k < NPER; k++)
            th[n++] = pth_spawn_on(s, attr, hammer, NULL);
    for (s = 0; s < n; s++)
        pth_join(th[s], NULL);

    pth_attr_destroy(attr);
    pth_mp_shutdown();

    printf("fair-mp: counter=%ld (want %d) %s\n",
           counter, NSCHED*NPER*ITER,
           counter == (long)NSCHED*NPER*ITER ? "OK" : "FAIL");
    return counter == (long)NSCHED*NPER*ITER ? 0 : 1;
}
