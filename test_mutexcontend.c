/*
**  test_mutexcontend.c -- high-contention microbenchmark for the cross-scheduler
**  spinlock that guards a pth_mutex.  Many worker threads all hammer ONE shared
**  pth_mutex; its internal spinlock is therefore the contended cross-scheduler
**  lock.  We report throughput with all workers on a SINGLE scheduler
**  (cooperative, so no cross-CPU contention on the spinlock) versus workers
**  spread over N schedulers (real contention), and verify the counter is exact.
**  The ratio quantifies the contended-lock overhead / anti-scaling; it is the
**  number to watch if the adaptive back-off is ever replaced by a parking lock.
**
**  Not a pass/fail test in the usual sense (a benchmark), but it does assert the
**  final counter, so a broken lock fails it.
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

#include "pth.h"
#include "test_common.h"

#ifndef MC_ITERS
#define MC_ITERS 50000
#endif
#define MC_MAXW 64

static pth_mutex_t mx;
static long        counter;
static long        iters;

static void *worker(void *arg)
{
    long i;
    (void)arg;
    for (i = 0; i < iters; i++) {
        pth_mutex_acquire(&mx, FALSE, NULL);
        counter++;                       /* the entire critical section */
        pth_mutex_release(&mx);
    }
    return NULL;
}

static double run(int nsched, int nworkers)
{
    pth_t th[MC_MAXW];
    pth_attr_t attr;
    struct timeval t0, t1;
    int i;

    counter = 0;
    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);
    gettimeofday(&t0, NULL);
    for (i = 0; i < nworkers; i++)
        th[i] = pth_spawn_on(i % nsched, attr, worker, NULL);
    for (i = 0; i < nworkers; i++)
        pth_join(th[i], NULL);
    gettimeofday(&t1, NULL);
    pth_attr_destroy(attr);

    return (double)(t1.tv_sec - t0.tv_sec)
         + (double)(t1.tv_usec - t0.tv_usec) / 1e6;
}

int main(void)
{
    int ns, nw, want;
    double t1, tN, ops;

    pth_init();
    want = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (want < 2) want = 2;
    if (want > 8) want = 8;
    if (!pth_mp_init(want)) {
        fprintf(stderr, "pth_mp_init(%d) failed (build without PTH_MP?)\n", want);
        pth_kill();
        return 1;
    }
    ns = pth_sched_count();
    iters = MC_ITERS;
    pth_mutex_init(&mx);
    nw = 2 * ns;
    if (nw > MC_MAXW) nw = MC_MAXW;
    ops = (double)nw * (double)iters;

    (void)run(1, nw);                    /* warm up */
    t1 = run(1, nw);                     /* all on scheduler 0: no cross-CPU contention */
    tN = run(ns, nw);                    /* spread over ns schedulers: real contention  */

    pth_mp_shutdown();

    printf("schedulers=%d workers=%d iters=%ld (total ops=%.0f)\n",
           ns, nw, iters, ops);
    printf("1-scheduler (uncontended): %8.3f s -> %10.0f ops/s\n", t1, ops / t1);
    printf("%d-scheduler (contended) : %8.3f s -> %10.0f ops/s\n", ns, tN, ops / tN);
    printf("contended / uncontended throughput: %.2fx\n", (ops / tN) / (ops / t1));

    if (counter != (long)nw * iters) {
        printf("counter=%ld (want %ld) -- MISMATCH (lock broken)\n",
               counter, (long)nw * iters);
        pth_kill();
        return 1;
    }
    printf("counter OK (%ld)\nMUTEXCONTEND DONE\n", counter);
    pth_kill();
    return 0;
}
