/* Cross-scheduler pth_cancel and pth_raise: the main thread on scheduler 0
   cancels / signals threads that live on other schedulers, verifying the
   request is carried out on the target's home scheduler via the inbox. */
#include "pth.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#define NSCHED 4

/* deferred-cancel target: loops hitting an explicit cancellation point */
static void *deferred_target(void *arg)
{
    for (;;) {
        pth_cancel_point();
        pth_yield(NULL);
    }
    return (void *)0x1;   /* never reached */
}

/* async-cancel target: enables async cancellation then blocks in a nap */
static void *async_target(void *arg)
{
    pth_cancel_state(PTH_CANCEL_ENABLE|PTH_CANCEL_ASYNCHRONOUS, NULL);
    for (;;)
        pth_nap(pth_time(10, 0));   /* long sleeps; cancelled mid-wait */
    return (void *)0x2;
}

/* raise target: waits for SIGUSR1 delivered per-thread, returns the signo */
static void *raise_target(void *arg)
{
    sigset_t set;
    int sig = -1;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pth_sigwait(&set, &sig);
    return (void *)(long)sig;
}

int main(void)
{
    pth_attr_t attr;
    pth_t t;
    void *ret;
    int fails = 0;

    pth_init();
    if (!pth_mp_init(NSCHED)) { fprintf(stderr, "no PTH_MP\n"); return 1; }
    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);

    /* 1: deferred cancellation of a thread on scheduler 1 */
    t = pth_spawn_on(1, attr, deferred_target, NULL);
    while (pth_nap(pth_time(0, 2000)), 0) ;      /* (no-op placeholder) */
    pth_nap(pth_time(0, 5000));                  /* let it start looping */
    if (pth_cancel(t) != TRUE) { printf("1 pth_cancel returned FALSE\n"); fails++; }
    ret = (void *)0x999;
    pth_join(t, &ret);
    printf("1 deferred cancel (sched1): ret=%s %s\n",
           ret==PTH_CANCELED?"PTH_CANCELED":"?", ret==PTH_CANCELED?"OK":"FAIL");
    if (ret != PTH_CANCELED) fails++;

    /* 2: asynchronous cancellation of a napping thread on scheduler 2 */
    t = pth_spawn_on(2, attr, async_target, NULL);
    pth_nap(pth_time(0, 5000));
    if (pth_cancel(t) != TRUE) { printf("2 pth_cancel returned FALSE\n"); fails++; }
    ret = (void *)0x999;
    pth_join(t, &ret);
    printf("2 async cancel (sched2)   : ret=%s %s\n",
           ret==PTH_CANCELED?"PTH_CANCELED":"?", ret==PTH_CANCELED?"OK":"FAIL");
    if (ret != PTH_CANCELED) fails++;

    /* 3: raise SIGUSR1 to a sigwaiting thread on scheduler 3 */
    t = pth_spawn_on(3, attr, raise_target, NULL);
    pth_nap(pth_time(0, 5000));                  /* let it enter sigwait */
    if (pth_raise(t, SIGUSR1) != TRUE) { printf("3 pth_raise returned FALSE\n"); fails++; }
    ret = (void *)0x999;
    pth_join(t, &ret);
    printf("3 raise SIGUSR1 (sched3)  : got=%ld want=%d %s\n",
           (long)ret, SIGUSR1, (long)ret==SIGUSR1?"OK":"FAIL");
    if ((long)ret != SIGUSR1) fails++;

    pth_attr_destroy(attr);
    pth_mp_shutdown();
    printf(fails==0 ? "ALL CANCEL/RAISE-MP TESTS PASSED\n" : "%d CANCEL/RAISE-MP TESTS FAILED\n", fails);
    return fails==0 ? 0 : 1;
}
