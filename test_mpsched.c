/* test_mpsched.c: cross-scheduler stress test for multi-scheduler Pth
   (requires a library built with PTH_MP) */

#include <stdio.h>
#include <stdlib.h>
#include "pth.h"

#define NSCHED    4
#define NWORKERS  3     /* per scheduler */
#define NITER     5000

static int fails = 0;

/* --- 1: cross-scheduler mutex contention --- */
static pth_mutex_t   mx = PTH_MUTEX_INIT;
static long          counter = 0;
static pth_barrier_t donebar;

static void *mx_worker(void *arg)
{
    int i;
    for (i = 0; i < NITER; i++) {
        pth_mutex_acquire(&mx, FALSE, NULL);
        counter++;
        if ((i % 256) == 0)
            pth_yield(NULL);    /* sometimes block while holding it */
        pth_mutex_release(&mx);
    }
    pth_barrier_reach(&donebar);
    return NULL;
}

/* --- 2: cross-scheduler cond producer/consumer --- */
static pth_mutex_t   qmx = PTH_MUTEX_INIT;
static pth_cond_t    qcv = PTH_COND_INIT;
static int           qbuf = 0, qdone = 0;
static long          produced = 0, consumed = 0;
static pth_barrier_t pcbar;

static void *producer(void *arg)
{
    int i;
    for (i = 0; i < NITER; i++) {
        pth_mutex_acquire(&qmx, FALSE, NULL);
        while (qbuf != 0)
            pth_cond_await(&qcv, &qmx, NULL);
        qbuf = 1;
        produced++;
        pth_cond_notify(&qcv, TRUE);
        pth_mutex_release(&qmx);
    }
    pth_mutex_acquire(&qmx, FALSE, NULL);
    qdone = 1;
    pth_cond_notify(&qcv, TRUE);
    pth_mutex_release(&qmx);
    pth_barrier_reach(&pcbar);
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
            pth_cond_notify(&qcv, TRUE);
        }
        else if (qdone) {
            pth_mutex_release(&qmx);
            break;
        }
        pth_mutex_release(&qmx);
    }
    pth_barrier_reach(&pcbar);
    return NULL;
}

/* --- 4: cross-scheduler join helper --- */
static void *fworker(void *arg)
{
    int i;
    /* do a little cooperative work so the remote joiner really blocks */
    for (i = 0; i < 5; i++)
        pth_yield(NULL);
    return (void *)0x5678;
}

/* --- 3: cross-scheduler rwlock --- */
static pth_rwlock_t  rwl = PTH_RWLOCK_INIT;
static long          shared_val = 0, read_errs = 0;
static pth_barrier_t rwbar;

static void *rw_writer(void *arg)
{
    int i;
    for (i = 0; i < 1000; i++) {
        pth_rwlock_acquire(&rwl, PTH_RWLOCK_RW, FALSE, NULL);
        shared_val++;
        if ((i % 64) == 0)
            pth_yield(NULL);
        shared_val++;
        pth_rwlock_release(&rwl);
    }
    pth_barrier_reach(&rwbar);
    return NULL;
}

static void *rw_reader(void *arg)
{
    int i;
    for (i = 0; i < 1000; i++) {
        long v1, v2;
        pth_rwlock_acquire(&rwl, PTH_RWLOCK_RD, FALSE, NULL);
        v1 = shared_val;
        pth_yield(NULL);
        v2 = shared_val;
        if (v1 != v2 || (v1 % 2) != 0)
            read_errs++;
        pth_rwlock_release(&rwl);
    }
    pth_barrier_reach(&rwbar);
    return NULL;
}

int main(int argc, char *argv[])
{
    pth_attr_t attr;
    int s, w;

    pth_init();
    if (!pth_mp_init(NSCHED)) {
        fprintf(stderr, "pth_mp_init(%d) failed (library built without PTH_MP?)\n", NSCHED);
        exit(1);
    }
    printf("schedulers: %d (main runs on #%d)\n", pth_sched_count(), pth_sched_id());
    if (pth_sched_count() != NSCHED || pth_sched_id() != 0)
        fails++;

    /* tests 1-3 use non-joinable workers whose completion is signalled
       through barriers; test 4 below uses its own joinable attribute to
       exercise cross-scheduler pth_join (phase 4) */
    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, FALSE);

    /* 1: mutex hammering from all schedulers */
    pth_barrier_init(&donebar, NSCHED*NWORKERS + 1);
    for (s = 0; s < NSCHED; s++)
        for (w = 0; w < NWORKERS; w++)
            if (pth_spawn_on(s, attr, mx_worker, NULL) == NULL) {
                fprintf(stderr, "spawn_on(%d) failed\n", s);
                exit(1);
            }
    pth_barrier_reach(&donebar);
    printf("mutex:   counter=%ld (want %d) %s\n", counter, NSCHED*NWORKERS*NITER,
           counter == (long)NSCHED*NWORKERS*NITER ? "OK" : "FAIL");
    if (counter != (long)NSCHED*NWORKERS*NITER) fails++;

    /* 2: producer on scheduler 1, consumer on scheduler 2 */
    pth_barrier_init(&pcbar, 3);
    pth_spawn_on(1, attr, producer, NULL);
    pth_spawn_on(2, attr, consumer, NULL);
    pth_barrier_reach(&pcbar);
    printf("cond:    produced=%ld consumed=%ld %s\n", produced, consumed,
           (produced == NITER && consumed == NITER) ? "OK" : "FAIL");
    if (!(produced == NITER && consumed == NITER)) fails++;

    /* 3: writer on scheduler 1, readers on schedulers 2, 3 and 0 */
    pth_barrier_init(&rwbar, 4 + 1);
    pth_spawn_on(1, attr, rw_writer, NULL);
    pth_spawn_on(2, attr, rw_reader, NULL);
    pth_spawn_on(3, attr, rw_reader, NULL);
    pth_spawn_on(0, attr, rw_reader, NULL);
    pth_barrier_reach(&rwbar);
    printf("rwlock:  val=%ld errs=%ld %s\n", shared_val, read_errs,
           (shared_val == 2000 && read_errs == 0) ? "OK" : "FAIL");
    if (!(shared_val == 2000 && read_errs == 0)) fails++;

    /* 4: cross-scheduler join now works (phase 4) */
    {
        pth_attr_t jattr;
        pth_t t;
        void *ret = NULL;
        jattr = pth_attr_new();
        pth_attr_set(jattr, PTH_ATTR_JOINABLE, TRUE);
        t = pth_spawn_on(1, jattr, fworker, NULL);   /* remote, joinable */
        if (t != NULL && pth_join(t, &ret) == TRUE && ret == (void *)0x5678)
            printf("join:    cross-scheduler join returned value OK\n");
        else {
            printf("join:    cross-scheduler join FAIL\n");
            fails++;
        }
        pth_attr_destroy(jattr);
    }

    /* shut the auxiliary schedulers down */
    if (!pth_mp_shutdown()) {
        printf("shutdown: FAIL\n");
        fails++;
    }
    else
        printf("shutdown: %d scheduler(s) left OK\n", pth_sched_count());
    if (pth_sched_count() != 1) fails++;

    pth_attr_destroy(attr);
    printf(fails == 0 ? "ALL MP TESTS PASSED\n" : "%d MP TESTS FAILED\n", fails);
    pth_kill();
    return fails == 0 ? 0 : 1;
}
