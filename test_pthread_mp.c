/* Proves the pthread emulation distributes threads across schedulers under
   PTH_MP: each worker records the OS-thread id (gettid) it runs on -- one
   per scheduler, no migration -- so a spread of distinct tids means real
   multi-scheduler placement. Also checks a pthread_mutex-protected counter
   stays correct under that real parallelism. */
#define _GNU_SOURCE
#include "pthread.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>

#define NTHREADS 16
#define NITER    5000

static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static long counter;
static long tids[NTHREADS];

static void *worker(void *arg)
{
    long idx = (long)arg;
    int i;
    tids[idx] = (long)syscall(SYS_gettid);
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
    int fails = 0;

    for (i = 0; i < NTHREADS; i++)
        if (pthread_create(&th[i], NULL, worker, (void *)i) != 0) {
            fprintf(stderr, "pthread_create %ld failed\n", i); return 1;
        }
    for (i = 0; i < NTHREADS; i++) {
        void *r = NULL;
        pthread_join(th[i], &r);
        if (r != (void *)(i + 100)) fails++;   /* return value delivered */
    }

    /* count distinct OS-thread ids the workers ran on */
    for (i = 0; i < NTHREADS; i++) {
        long j, seen = 0;
        for (j = 0; j < i; j++) if (tids[j] == tids[i]) { seen = 1; break; }
        if (!seen) distinct++;
    }

    printf("counter        : %ld (want %d) %s\n", counter, NTHREADS*NITER,
           counter == (long)NTHREADS*NITER ? "OK" : "FAIL");
    if (counter != (long)NTHREADS*NITER) fails++;
    printf("return values  : %s\n", fails==0 || counter==(long)NTHREADS*NITER ? "delivered" : "?");
    printf("distinct OS thr: %ld across %d pthreads (%d CPUs online)\n",
           distinct, NTHREADS, (int)sysconf(_SC_NPROCESSORS_ONLN));
    printf("multi-scheduler: %s\n", distinct > 1 ? "YES (threads spread over schedulers)" : "NO (all on one)");
    if (distinct <= 1) fails++;

    printf(fails==0 ? "ALL PTHREAD-MP TESTS PASSED\n" : "%d PTHREAD-MP TESTS FAILED\n", fails);
    return fails==0 ? 0 : 1;
}
