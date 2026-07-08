/* Proves the pthread emulation distributes threads across schedulers under
   PTH_MP. Rather than an OS-thread id (gettid is Linux-only, not portable to
   macOS/FreeBSD), each worker records the id of the *scheduler* it runs on via
   pth_sched_id() -- threads never migrate, so a spread of distinct scheduler
   ids means real multi-scheduler placement. Also checks a pthread_mutex-
   protected counter stays correct under that parallelism. */
#include "pthread.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Pth native introspection, declared directly to avoid mixing pth.h with the
   pthread emulation header (both are provided by the linked libpth). */
extern int pth_sched_id(void);
extern int pth_sched_count(void);

#define NTHREADS 16
#define NITER    5000

static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static long counter;
static int  sched_of[NTHREADS];

static void *worker(void *arg)
{
    long idx = (long)arg;
    int i;
    sched_of[idx] = pth_sched_id();          /* which scheduler runs us */
    for (i = 0; i < NITER; i++) {
        pthread_mutex_lock(&mx);
        counter++;
        pthread_mutex_unlock(&mx);
    }
    return (void *)(idx + 100);
}

int main(void)
{
    pthread_t th[NTHREADS];
    long i, distinct = 0;
    int nsched, fails = 0;

    for (i = 0; i < NTHREADS; i++)
        if (pthread_create(&th[i], NULL, worker, (void *)i) != 0) {
            fprintf(stderr, "pthread_create %ld failed\n", i); return 1;
        }
    for (i = 0; i < NTHREADS; i++) {
        void *r = NULL;
        pthread_join(th[i], &r);
        if (r != (void *)(i + 100)) fails++;   /* return value delivered */
    }
    nsched = pth_sched_count();

    for (i = 0; i < NTHREADS; i++) {
        long j, seen = 0;
        for (j = 0; j < i; j++) if (sched_of[j] == sched_of[i]) { seen = 1; break; }
        if (!seen) distinct++;
    }

    printf("counter        : %ld (want %d) %s\n", counter, NTHREADS*NITER,
           counter == (long)NTHREADS*NITER ? "OK" : "FAIL");
    if (counter != (long)NTHREADS*NITER) fails++;
    printf("return values  : %s\n", fails==0 || counter==(long)NTHREADS*NITER ? "delivered" : "?");
    printf("schedulers     : %d running (%d CPUs online)\n",
           nsched, (int)sysconf(_SC_NPROCESSORS_ONLN));
    printf("distinct scheds: %ld across %d pthreads\n", distinct, NTHREADS);

    if (nsched > 1) {
        printf("multi-scheduler: %s\n",
               distinct > 1 ? "YES (threads spread over schedulers)"
                            : "NO (all on one -- distribution broken)");
        if (distinct <= 1) fails++;
    }
    else {
        printf("multi-scheduler: single scheduler only "
               "(pth_mp_init created 1; check aux-scheduler startup)\n");
        fails++;
    }

    printf(fails==0 ? "ALL PTHREAD-MP TESTS PASSED\n" : "%d PTHREAD-MP TESTS FAILED\n", fails);
    return fails==0 ? 0 : 1;
}
