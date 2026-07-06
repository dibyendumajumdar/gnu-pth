/*
**  test_mutexfair.c -- exercise the PTH_MUTEX_FAIR ownership-handoff mode.
**
**  The pth main thread acquires the mutex, spawns N workers (all of which
**  block trying to acquire, so all N end up parked on the wait queue), then
**  releases once. Barging leaves the lock free so the just-woken worker
**  monopolises it (long consecutive run in the acquisition log = starvation);
**  fair hands ownership directly to the head waiter so a looping worker must
**  re-queue behind the others (strict round-robin, run length 1).
*/
#include "pth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N      4
#define ITER   50
#define LOGMAX (N*ITER)

static pth_mutex_t mtx;
static int  order[LOGMAX];
static int  nlog;
static long counter;

static void *worker(void *arg)
{
    long id = (long)arg;
    int i;
    for (i = 0; i < ITER; i++) {
        pth_mutex_acquire(&mtx, FALSE, NULL);
        if (nlog < LOGMAX)
            order[nlog++] = (int)id;
        counter++;               /* protected write: verifies mutual exclusion */
        pth_mutex_release(&mtx);
        /* deliberately NO pth_yield here: a barging hog re-grabs at once */
    }
    return NULL;
}

static int max_run(void)
{
    int i, run = 1, mx = (nlog > 0 ? 1 : 0);
    for (i = 1; i < nlog; i++) {
        if (order[i] == order[i-1]) { run++; if (run > mx) mx = run; }
        else run = 1;
    }
    return mx;
}

static int run_case(pth_attr_t attr, int fair, int *out_maxrun)
{
    pth_t th[N];
    long i;

    pth_mutex_init(&mtx);
    pth_mutex_setfair(&mtx, fair);
    memset(order, 0, sizeof(order));
    nlog = 0;
    counter = 0;

    pth_mutex_acquire(&mtx, FALSE, NULL);          /* hold it */
    for (i = 0; i < N; i++)
        th[i] = pth_spawn(attr, worker, (void *)i);
    for (i = 0; i < 4*N; i++)                       /* let all N park */
        pth_yield(NULL);
    pth_mutex_release(&mtx);                        /* policy governs from here */

    for (i = 0; i < N; i++)
        pth_join(th[i], NULL);

    *out_maxrun = max_run();
    return (counter == (long)N*ITER && nlog == N*ITER);
}

int main(void)
{
    pth_attr_t attr;
    pth_mutex_t r;
    int rc_barge, rc_fair, run_barge = 0, run_fair = 0, fails = 0;

    pth_init();
    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);

    rc_barge = run_case(attr, FALSE, &run_barge);
    printf("barging: acquisitions=%d counter_ok=%s max_consecutive_run=%d\n",
           nlog, rc_barge ? "yes" : "NO", run_barge);
    rc_fair = run_case(attr, TRUE, &run_fair);
    printf("fair:    acquisitions=%d counter_ok=%s max_consecutive_run=%d\n",
           nlog, rc_fair ? "yes" : "NO", run_fair);

    if (!rc_barge || !rc_fair) {
        printf("FAIL: counter/log mismatch (mutual exclusion broken)\n"); fails++;
    }
    if (run_fair > 2) {
        printf("FAIL: fair run length %d > 2 (not round-robin)\n", run_fair); fails++;
    }
    if (run_barge <= run_fair) {
        printf("FAIL: barging run %d not larger than fair run %d\n", run_barge, run_fair); fails++;
    }

    /* fair mutex must still support recursion and tryonly */
    pth_mutex_init(&r);
    pth_mutex_setfair(&r, TRUE);
    if (pth_mutex_acquire(&r, FALSE, NULL) != TRUE ||
        pth_mutex_acquire(&r, FALSE, NULL) != TRUE) {
        printf("FAIL: fair recursive acquire\n"); fails++;
    }
    pth_mutex_release(&r);
    pth_mutex_release(&r);
    if (pth_mutex_acquire(&r, TRUE, NULL) != TRUE) {
        printf("FAIL: fair tryonly on free mutex\n"); fails++;
    }
    pth_mutex_release(&r);
    printf("recursion+tryonly on fair mutex: OK\n");

    pth_attr_destroy(attr);
    pth_kill();
    printf(fails == 0 ? "ALL FAIR-MUTEX TESTS PASSED\n" : "%d FAIR-MUTEX TESTS FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
