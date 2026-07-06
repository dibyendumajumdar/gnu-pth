/* stress test for the waiter-queue based pth sync primitives */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pth.h"

#define NTHREADS   10
#define NITER    2000

/* --- mutex contention --- */
static pth_mutex_t mx = PTH_MUTEX_INIT;
static long counter = 0;
static void *mx_worker(void *arg)
{
    int i;
    for (i = 0; i < NITER; i++) {
        pth_mutex_acquire(&mx, FALSE, NULL);
        counter++;
        if (i % 64 == 0)
            pth_yield(NULL);
        pth_mutex_release(&mx);
    }
    return NULL;
}

/* --- cond producer/consumer --- */
static pth_mutex_t qmx = PTH_MUTEX_INIT;
static pth_cond_t  qcv = PTH_COND_INIT;
static int qbuf = 0, qdone = 0;
static long consumed = 0, produced = 0;
static void *producer(void *arg)
{
    int i;
    for (i = 0; i < NITER; i++) {
        pth_mutex_acquire(&qmx, FALSE, NULL);
        while (qbuf != 0) {
            pth_mutex_release(&qmx);
            pth_yield(NULL);
            pth_mutex_acquire(&qmx, FALSE, NULL);
        }
        qbuf = 1;
        produced++;
        pth_cond_notify(&qcv, FALSE);
        pth_mutex_release(&qmx);
    }
    pth_mutex_acquire(&qmx, FALSE, NULL);
    qdone = 1;
    pth_cond_notify(&qcv, TRUE);
    pth_mutex_release(&qmx);
    return NULL;
}
static void *consumer(void *arg)
{
    for (;;) {
        pth_mutex_acquire(&qmx, FALSE, NULL);
        while (qbuf == 0 && !qdone)
            pth_cond_await(&qcv, &qmx, NULL);
        if (qbuf == 1) {
            qbuf = 0;
            consumed++;
        }
        else if (qdone) {
            pth_mutex_release(&qmx);
            break;
        }
        pth_mutex_release(&qmx);
    }
    return NULL;
}

/* --- barrier cycles --- */
#define BARN 5
#define BARCYCLES 200
static pth_barrier_t bar;
static long barhits[BARN];
static void *bar_worker(void *arg)
{
    long id = (long)arg;
    int i;
    for (i = 0; i < BARCYCLES; i++) {
        pth_barrier_reach(&bar);
        barhits[id]++;
    }
    return NULL;
}

/* --- rwlock --- */
static pth_rwlock_t rwl = PTH_RWLOCK_INIT;
static long shared_val = 0, read_errs = 0;
static void *rw_writer(void *arg)
{
    int i;
    for (i = 0; i < 500; i++) {
        pth_rwlock_acquire(&rwl, PTH_RWLOCK_RW, FALSE, NULL);
        shared_val++;
        pth_yield(NULL);
        shared_val++;
        pth_rwlock_release(&rwl);
    }
    return NULL;
}
static void *rw_reader(void *arg)
{
    int i;
    for (i = 0; i < 500; i++) {
        long v1, v2;
        pth_rwlock_acquire(&rwl, PTH_RWLOCK_RD, FALSE, NULL);
        v1 = shared_val;
        pth_yield(NULL);
        v2 = shared_val;
        if (v1 != v2 || (v1 % 2) != 0)
            read_errs++;
        pth_rwlock_release(&rwl);
    }
    return NULL;
}

/* --- mutex acquire timeout via ev_extra --- */
static pth_mutex_t slowmx = PTH_MUTEX_INIT;
static void *holder(void *arg)
{
    pth_mutex_acquire(&slowmx, FALSE, NULL);
    pth_nap(pth_time(1, 0));
    pth_mutex_release(&slowmx);
    return NULL;
}

int main(int argc, char *argv[])
{
    pth_t tid[NTHREADS];
    pth_t prod, cons, hold;
    pth_attr_t attr;
    pth_event_t ev;
    int i, rc;
    int fails = 0;

    pth_init();
    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);

    /* 1: mutex contention */
    for (i = 0; i < NTHREADS; i++)
        tid[i] = pth_spawn(attr, mx_worker, NULL);
    for (i = 0; i < NTHREADS; i++)
        pth_join(tid[i], NULL);
    printf("mutex:   counter=%ld (want %d) %s\n", counter, NTHREADS*NITER,
           counter == (long)NTHREADS*NITER ? "OK" : "FAIL");
    if (counter != (long)NTHREADS*NITER) fails++;

    /* 2: cond producer/consumer */
    prod = pth_spawn(attr, producer, NULL);
    cons = pth_spawn(attr, consumer, NULL);
    pth_join(prod, NULL);
    pth_join(cons, NULL);
    printf("cond:    produced=%ld consumed=%ld %s\n", produced, consumed,
           (produced == NITER && consumed == NITER) ? "OK" : "FAIL");
    if (!(produced == NITER && consumed == NITER)) fails++;

    /* 3: barrier cycles */
    pth_barrier_init(&bar, BARN);
    for (i = 0; i < BARN; i++)
        tid[i] = pth_spawn(attr, bar_worker, (void *)(long)i);
    for (i = 0; i < BARN; i++)
        pth_join(tid[i], NULL);
    rc = 0;
    for (i = 0; i < BARN; i++)
        if (barhits[i] != BARCYCLES) rc++;
    printf("barrier: cycles=%d threads=%d %s\n", BARCYCLES, BARN, rc == 0 ? "OK" : "FAIL");
    if (rc != 0) fails++;

    /* 4: rwlock */
    tid[0] = pth_spawn(attr, rw_writer, NULL);
    for (i = 1; i <= 4; i++)
        tid[i] = pth_spawn(attr, rw_reader, NULL);
    for (i = 0; i <= 4; i++)
        pth_join(tid[i], NULL);
    printf("rwlock:  val=%ld errs=%ld %s\n", shared_val, read_errs,
           (shared_val == 1000 && read_errs == 0) ? "OK" : "FAIL");
    if (!(shared_val == 1000 && read_errs == 0)) fails++;

    /* 5: mutex acquire with timeout */
    hold = pth_spawn(attr, holder, NULL);
    pth_yield(NULL); /* let holder grab the mutex */
    ev = pth_event(PTH_EVENT_TIME, pth_timeout(0, 100000)); /* 100ms */
    rc = pth_mutex_acquire(&slowmx, FALSE, ev);
    printf("timeout: acquire=%d errno=%s %s\n", rc, rc ? "-" : "EINTR-expected",
           (rc == FALSE) ? "OK" : "FAIL");
    if (rc != FALSE) fails++;
    pth_event_free(ev, PTH_FREE_THIS);
    /* now wait for it properly (holder releases after 1s) */
    rc = pth_mutex_acquire(&slowmx, FALSE, NULL);
    if (rc) pth_mutex_release(&slowmx);
    printf("blockacq: rc=%d %s\n", rc, rc ? "OK" : "FAIL");
    if (!rc) fails++;
    pth_join(hold, NULL);

    pth_attr_destroy(attr);
    pth_kill();
    printf(fails == 0 ? "ALL SYNC TESTS PASSED\n" : "%d SYNC TESTS FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
