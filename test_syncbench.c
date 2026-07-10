/*
**  test_syncbench.c -- throughput of a synchronization hand-off when both
**  parties live on the SAME scheduler versus on DIFFERENT schedulers.
**
**  Two threads ping-pong a token through a mutex + a pair of condition
**  variables (directed wakeup). Same-scheduler, a cond_notify wakes a local
**  green thread with no OS-thread involvement. Cross-scheduler, it must post to
**  the peer scheduler's wakeup inbox and kick its self-pipe so it breaks out of
**  epoll_wait()/kevent()/select() -- so this measures the price of the
**  cross-scheduler wakeup substrate. Reports hand-offs/second for each and the
**  ratio.  Not pass/fail (a benchmark); exits 0 unless the run breaks.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "pth.h"
#include "test_common.h"

#ifndef SB_ITERS
#define SB_ITERS 100000          /* per worker; total hand-offs = 2*SB_ITERS */
#endif

static pth_mutex_t m;
static pth_cond_t  c[2];
static int         turn;
static long        iters;

static void *worker(void *arg)
{
    long id = (long)arg;
    long i;
    for (i = 0; i < iters; i++) {
        pth_mutex_acquire(&m, FALSE, NULL);
        while (turn != id)
            pth_cond_await(&c[id], &m, NULL);
        turn = 1 - (int)id;
        pth_cond_notify(&c[1 - id], FALSE);   /* wake exactly the peer */
        pth_mutex_release(&m);
    }
    return NULL;
}

static double run(int sched_a, int sched_b)
{
    pth_t a, b;
    pth_attr_t attr;
    struct timeval t0, t1;
    double secs;

    turn = 0;
    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);

    gettimeofday(&t0, NULL);
    a = pth_spawn_on(sched_a, attr, worker, (void *)0L);
    b = pth_spawn_on(sched_b, attr, worker, (void *)1L);
    if (a == NULL || b == NULL) { perror("pth_spawn_on"); exit(1); }
    pth_join(a, NULL);
    pth_join(b, NULL);
    gettimeofday(&t1, NULL);
    pth_attr_destroy(attr);

    secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_usec - t0.tv_usec) / 1e6;
    return secs;
}

int main(void)
{
    int i, ns;
    double intra, cross;
    double h = (double)(2L * SB_ITERS);

    pth_init();
    if (!pth_mp_init(2)) {
        fprintf(stderr, "pth_mp_init(2) failed (build without PTH_MP?)\n");
        pth_kill();
        return 1;
    }
    ns = pth_sched_count();
    iters = SB_ITERS;

    pth_mutex_init(&m);
    for (i = 0; i < 2; i++)
        pth_cond_init(&c[i]);

    (void)run(0, 0);                 /* warm up caches/first-touch */
    intra = run(0, 0);               /* both on scheduler 0        */
    cross = (ns > 1) ? run(0, 1) : 0.0;   /* one each on 0 and 1   */

    pth_mp_shutdown();
    pth_kill();

    printf("schedulers running        : %d\n", ns);
    printf("hand-offs per measurement : %.0f (2 x %d)\n", h, SB_ITERS);
    printf("intra-scheduler           : %8.3f s  ->  %10.0f hand-offs/s\n",
           intra, h / intra);
    if (ns > 1) {
        printf("cross-scheduler           : %8.3f s  ->  %10.0f hand-offs/s\n",
               cross, h / cross);
        printf("intra/cross throughput    : %.1fx  (cross pays the inbox + self-pipe wakeup)\n",
               (h / intra) / (h / cross));
    }
    else {
        printf("cross-scheduler           : skipped (only 1 scheduler available)\n");
    }
    printf("SYNCBENCH DONE\n");
    return 0;
}
