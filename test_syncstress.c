/*
**  test_syncstress.c -- cross-scheduler synchronization stress, intended to be
**  run under ThreadSanitizer (`-fsanitize=thread`).  Many worker threads spread
**  across several schedulers hammer shared data through pth_mutex and
**  pth_rwlock. The protected data (a counter, a shared array) is touched from
**  multiple OS threads; if any primitive were missing its acquire/release
**  ordering, TSan would report a data race on that data. A clean TSan run plus
**  the exact final counter is the evidence that the M:N memory model holds.
*/

#include <stdio.h>
#include <stdlib.h>

#include "pth.h"
#include "test_common.h"

#define NW    8        /* workers of each kind */
#define NITER 3000     /* iterations per worker (kept modest: TSan is ~15x slow) */
#define NA    16       /* rwlock-protected array size */

static pth_mutex_t  mx;
static pth_rwlock_t rw;
static long         counter;     /* mutex-protected */
static long         shared[NA];  /* rwlock-protected */

static void *mworker(void *arg)
{
    int i;
    (void)arg;
    for (i = 0; i < NITER; i++) {
        pth_mutex_acquire(&mx, FALSE, NULL);
        counter++;                       /* racy iff mx lacks a barrier */
        pth_mutex_release(&mx);
    }
    return NULL;
}

static void *rwworker(void *arg)
{
    int i, j;
    (void)arg;
    for (i = 0; i < NITER; i++) {
        if ((i & 7) == 0) {              /* ~1/8 writers */
            pth_rwlock_acquire(&rw, PTH_RWLOCK_RW, FALSE, NULL);
            for (j = 0; j < NA; j++)
                shared[j]++;
            pth_rwlock_release(&rw);
        }
        else {                           /* readers */
            long sum = 0;
            pth_rwlock_acquire(&rw, PTH_RWLOCK_RD, FALSE, NULL);
            for (j = 0; j < NA; j++)
                sum += shared[j];
            pth_rwlock_release(&rw);
            if (sum == 0x7fffffff) printf(" ");   /* force a real read */
        }
    }
    return NULL;
}

int main(void)
{
    pth_t th[2 * NW];
    pth_attr_t attr;
    int i, ns, fails = 0;

    pth_init();
    if (!pth_mp_init(4)) {
        fprintf(stderr, "pth_mp_init(4) failed (build without PTH_MP?)\n");
        pth_kill();
        return 1;
    }
    ns = pth_sched_count();
    pth_mutex_init(&mx);
    pth_rwlock_init(&rw);

    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);
    for (i = 0; i < NW; i++)
        th[i] = pth_spawn_on(i % ns, attr, mworker, NULL);
    for (i = 0; i < NW; i++)
        th[NW + i] = pth_spawn_on(i % ns, attr, rwworker, (void *)(long)i);
    pth_attr_destroy(attr);

    for (i = 0; i < 2 * NW; i++)
        pth_join(th[i], NULL);
    pth_mp_shutdown();

    if (counter != (long)NW * NITER) {
        printf("counter=%ld (want %d) -- FAIL\n", counter, NW * NITER);
        fails++;
    }
    printf("counter=%ld  shared[0]=%ld  over %d schedulers\n",
           counter, shared[0], ns);

    pth_kill();
    printf(fails == 0 ? "ALL SYNCSTRESS TESTS PASSED\n" : "SYNCSTRESS FAILED\n");
    return fails == 0 ? 0 : 1;
}
