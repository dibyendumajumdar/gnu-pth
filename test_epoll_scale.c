/*
**  test_epoll_scale.c -- scalability / persistent-registration test for the
**  multi-scheduler fd backend (exercises PTH_SCHED_EPOLL in particular, but is
**  backend-agnostic and also passes under poll(2)/select(2)).
**
**  Many reader threads -- spread across 3 schedulers -- each park in pth_read()
**  on their own socketpair.  Only once every reader is genuinely blocked (so
**  its descriptor sits in the kernel interest set) does the main thread write a
**  distinct byte to each pipe.  Every reader must wake with exactly its own
**  byte: this checks that a large, persistent set of registered descriptors is
**  woken *selectively* and correctly.
**
**  A second round closes every socketpair and opens fresh ones -- which reuses
**  the same descriptor numbers -- then repeats.  That drives the epoll backend
**  through the stale-registration / EPOLL_CTL self-healing paths.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "pth.h"
#include "test_common.h"

#define NPAIRS   120
#define NSCHED   3

static int  sv[NPAIRS][2];
static int  got[NPAIRS];

static unsigned char patt(int round, int i)
{
    return (unsigned char)((round * 97 + i * 3 + 7) & 0xFF);
}

static void *reader(void *arg)
{
    long i = (long)arg;
    unsigned char b = 0;
    ssize_t n = pth_read(sv[i][0], &b, 1);
    got[i] = (n == 1) ? (int)b : -1;
    return NULL;
}

static int round_run(int round)
{
    pth_attr_t attr;
    pth_t th[NPAIRS];
    int i, ns, fails = 0;

    ns = pth_sched_count();

    for (i = 0; i < NPAIRS; i++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv[i]) < 0) {
            perror("socketpair");
            return NPAIRS;
        }
        got[i] = -2;   /* sentinel: reader never ran */
    }

    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);
    for (i = 0; i < NPAIRS; i++) {
        th[i] = pth_spawn_on(i % ns, attr, reader, (void *)(long)i);
        if (th[i] == NULL) { perror("pth_spawn_on"); pth_attr_destroy(attr); return NPAIRS; }
    }
    pth_attr_destroy(attr);

    /* let every reader reach its blocking pth_read (so its fd is registered) */
    pth_nap(pth_time(0, 300000));   /* 300ms */

    /* now wake them all, each with its own distinct byte */
    for (i = 0; i < NPAIRS; i++) {
        unsigned char b = patt(round, i);
        if (pth_write(sv[i][1], &b, 1) != 1)
            fails++;
    }

    for (i = 0; i < NPAIRS; i++)
        pth_join(th[i], NULL);

    for (i = 0; i < NPAIRS; i++) {
        if (got[i] != (int)patt(round, i)) {
            if (fails < 8)
                printf("  pair %d: got %d, wanted %d\n", i, got[i], (int)patt(round, i));
            fails++;
        }
        close(sv[i][0]);
        close(sv[i][1]);
    }
    printf("round %d: %d readers over %d schedulers -> %s\n",
           round, NPAIRS, ns, fails == 0 ? "OK" : "MISMATCH");
    return fails;
}

int main(void)
{
    int fails = 0;

    pth_init();
    if (!pth_mp_init(NSCHED)) {
        fprintf(stderr, "pth_mp_init(%d) failed (library built without PTH_MP?)\n", NSCHED);
        pth_kill();
        return 1;
    }
    printf("TEST_EPOLL_SCALE: %d concurrent fd waiters, 2 rounds (round 2 reuses fds)\n",
           NPAIRS);

    fails += round_run(0);
    fails += round_run(1);   /* fresh socketpairs reuse the just-closed fd numbers */

    pth_mp_shutdown();
    pth_kill();

    printf(fails == 0 ? "ALL EPOLL-SCALE TESTS PASSED\n"
                      : "%d EPOLL-SCALE TESTS FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
