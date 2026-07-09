/*
**  GNU Pth - The GNU Portable Threads
**  Copyright (c) 1999-2006 Ralf S. Engelschall <rse@engelschall.com>
**
**  This file is part of GNU Pth, a non-preemptive thread scheduling
**  library which can be found at http://www.gnu.org/software/pth/.
**
**  This library is free software; you can redistribute it and/or
**  modify it under the terms of the GNU Lesser General Public
**  License as published by the Free Software Foundation; either
**  version 2.1 of the License, or (at your option) any later version.
**
**  This library is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
**  Lesser General Public License for more details.
**
**  You should have received a copy of the GNU Lesser General Public
**  License along with this library; if not, write to the Free Software
**  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
**  USA, or contact Ralf S. Engelschall <rse@engelschall.com>.
**
**  test_philo.c: Pth test application showing the 5 philosopher problem
*/
                             /* ``It's not enough to be a great programmer;
                                  you have to find a great problem.''
                                                -- Charles Simonyi  */

/*
 *  Reference: E.W. Dijkstra,
 *             ``Hierarchical Ordering of Sequential Processes'',
 *             Acta Informatica 1, 1971, 115-138.
 *
 *  Run with no argument for the classic interactive demonstration (single
 *  scheduler). Run as `test_philo mp' for a bounded multi-scheduler test: 3
 *  scheduler OS threads with the 5 philosophers spread across them, so the
 *  shared fork mutex and the per-philosopher condition variables are exercised
 *  across schedulers. It checks that every philosopher makes progress (no
 *  deadlock, no starvation) and then cancels/joins them across schedulers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>

#include "pth.h"

#include "test_common.h"

#define PHILNUM 5
#define MP_SCHEDS 3

typedef enum {
    thinking,
    hungry,
    eating
} philstat;

static const char *philstatstr[3] = {
    "thinking",
    "hungry  ",
    "EATING  "
};

typedef struct tablestruct {
    pth_t       tid[PHILNUM];
    int         self[PHILNUM];
    pth_mutex_t mutex;
    pth_cond_t  condition[PHILNUM];
    philstat    status[PHILNUM];
    long        eaten[PHILNUM];   /* meals per philosopher (MP-mode liveness) */
    int         sched[PHILNUM];   /* scheduler each philosopher runs on       */
} table;

static table *tab;
static int    g_mp = 0;          /* multi-scheduler mode */

static void printstate(void)
{
    int i;

    for (i = 0; i < PHILNUM; i++)
        printf("| %s ", philstatstr[(int)(tab->status)[i]]);
    printf("|\n");
    return;
}

static void think(unsigned int who)
{
    if (g_mp)
        pth_nap(pth_time(0, (who + 1) * 5000));   /* (who+1)*5ms */
    else
        pth_sleep(who + 1);
}
static void eat(unsigned int who)
{
    if (g_mp)
        pth_nap(pth_time(0, 5000));               /* 5ms */
    else
        pth_sleep(1);
    (void)who;
}

static int test(unsigned int i)
{
    if (   ((tab->status)[i] == hungry)
        && ((tab->status)[(i + 1) % PHILNUM] != eating)
        && ((tab->status)[(i - 1 + PHILNUM) % PHILNUM] != eating)) {
        (tab->status)[i] = eating;
        pth_cond_notify(&((tab->condition)[i]), FALSE);
        return TRUE;
    }
    return FALSE;
}

static void pickup(unsigned int k)
{
    pth_mutex_acquire(&(tab->mutex), FALSE, NULL);
    (tab->status)[k] = hungry;
    if (!g_mp) printstate();
    if (!test(k))
        pth_cond_await(&((tab->condition)[k]), &(tab->mutex), NULL);
    (tab->eaten)[k]++;            /* we are now eating (protected by mutex) */
    if (!g_mp) printstate();
    pth_mutex_release(&(tab->mutex));
    return;
}

static void putdown(unsigned int k)
{
    pth_mutex_acquire(&(tab->mutex), FALSE, NULL);
    (tab->status)[k] = thinking;
    if (!g_mp) printstate();
    test((k + 1) % PHILNUM);
    test((k - 1 + PHILNUM) % PHILNUM);
    pth_mutex_release(&(tab->mutex));
    return;
}

static void *philosopher(void *_who)
{
    unsigned int *who = (unsigned int *)_who;

    if (g_mp)
        (tab->sched)[*who] = pth_sched_id();   /* which scheduler runs us */

    /* For simplicity, all philosophers eat for the same amount of time
       and think for a time that is simply related to their position at
       the table. The parameter who identifies the philosopher: 0,1,2,.. */
    for (;;) {
        think(*who);
        pickup((*who));
        eat(*who);
        putdown((*who));
    }
    return NULL;
}

/* ---- bounded multi-scheduler test -------------------------------------- */
static int run_mp(void)
{
    int i, ns, fails = 0;
    pth_attr_t attr;

    if (!pth_mp_init(MP_SCHEDS)) {
        fprintf(stderr, "pth_mp_init(%d) failed (library built without PTH_MP?)\n", MP_SCHEDS);
        return 1;
    }
    ns = pth_sched_count();
    printf("TEST_PHILO (multi-scheduler): %d philosophers over %d schedulers\n",
           PHILNUM, ns);

    tab = (table *)malloc(sizeof(table));
    memset(tab, 0, sizeof(*tab));
    pth_mutex_init(&(tab->mutex));
    for (i = 0; i < PHILNUM; i++) {
        (tab->self)[i]   = i;
        (tab->status)[i] = thinking;
        pth_cond_init(&((tab->condition)[i]));
    }

    /* spread the philosophers across the schedulers; joinable so we can
       reap them by tid across schedulers afterwards */
    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);
    for (i = 0; i < PHILNUM; i++) {
        (tab->tid)[i] = pth_spawn_on(i % ns, attr, philosopher, &((tab->self)[i]));
        if ((tab->tid)[i] == NULL) { perror("pth_spawn_on"); return 1; }
    }
    pth_attr_destroy(attr);

    /* let them dine for a bounded while */
    pth_nap(pth_time(3, 0));

    /* cross-scheduler cancel + join of every philosopher */
    for (i = 0; i < PHILNUM; i++)
        pth_cancel((tab->tid)[i]);
    for (i = 0; i < PHILNUM; i++)
        pth_join((tab->tid)[i], NULL);

    pth_mp_shutdown();

    /* report + verdict: everyone must have eaten (liveness / no deadlock) */
    for (i = 0; i < PHILNUM; i++) {
        printf("philosopher %d: sched %d, ate %ld times %s\n",
               i, (tab->sched)[i], (tab->eaten)[i],
               (tab->eaten)[i] > 0 ? "OK" : "FAIL (starved/deadlocked)");
        if ((tab->eaten)[i] <= 0) fails++;
    }
    free(tab);
    printf(fails == 0 ? "ALL PHILO-MP TESTS PASSED\n" : "%d PHILO-MP TESTS FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}

int main(int argc, char *argv[])
{
    int i;
    sigset_t ss;
    int sig;
    pth_event_t ev;

    /* initialize Pth library */
    pth_init();

    if (argc > 1 && strcmp(argv[1], "mp") == 0) {
        int rc;
        g_mp = 1;
        rc = run_mp();
        pth_kill();
        return rc;
    }

    /* display test program header */
    printf("This is TEST_PHILO, a Pth test showing the Five Dining Philosophers\n");
    printf("\n");
    printf("This is a demonstration showing the famous concurrency problem of the\n");
    printf("Five Dining Philosophers as analysed 1965 by E.W.Dijkstra:\n");
    printf("\n");
    printf("Five philosophers are sitting around a round table, each with a bowl of\n");
    printf("Chinese food in front of him. Between periods of talking they may start\n");
    printf("eating whenever they want to, with their bowls being filled frequently.\n");
    printf("But there are only five chopsticks available, one each to the left of\n");
    printf("each bowl - and for eating Chinese food one needs two chopsticks. When\n");
    printf("a philosopher wants to start eating, he must pick up the chopstick to\n");
    printf("the left of his bowl and the chopstick to the right of his bowl. He\n");
    printf("may find, however, that either one (or even both) of the chopsticks is\n");
    printf("unavailable as it is being used by another philosopher sitting on his\n");
    printf("right or left, so he has to wait.\n");
    printf("\n");
    printf("This situation shows classical contention under concurrency (the\n");
    printf("philosophers want to grab the chopsticks) and the possibility of a\n");
    printf("deadlock (all philosophers wait that the chopstick to their left becomes\n");
    printf("available).\n");
    printf("\n");
    printf("(Hint: run `test_philo mp' for a bounded multi-scheduler variant.)\n");
    printf("\n");
    printf("The demonstration runs max. 60 seconds. To stop before, press CTRL-C.\n");
    printf("\n");
    printf("+----P1----+----P2----+----P3----+----P4----+----P5----+\n");

    /* initialize the control table */
    tab = (table *)malloc(sizeof(table));
    memset(tab, 0, sizeof(*tab));
    if (!pth_mutex_init(&(tab->mutex))) {
        perror("pth_mutex_init");
        exit(1);
    }
    for (i = 0; i < PHILNUM; i++) {
        (tab->self)[i] = i;
        (tab->status)[i] = thinking;
        if (!pth_cond_init(&((tab->condition)[i]))) {
            perror("pth_cond_init");
            exit(1);
        }
    }

    /* spawn the philosopher threads */
    for (i = 0; i < PHILNUM; i++) {
        if (((tab->tid)[i] =
              pth_spawn(PTH_ATTR_DEFAULT, philosopher,
                        &((tab->self)[i]))) == NULL) {
            perror("pth_spawn");
            exit(1);
        }
    }

    /* wait until 60 seconds have elapsed or CTRL-C was pressed */
    sigemptyset(&ss);
    sigaddset(&ss, SIGINT);
    ev = pth_event(PTH_EVENT_TIME, pth_timeout(60,0));
    pth_sigwait_ev(&ss, &sig, ev);
    pth_event_free(ev, PTH_FREE_ALL);

    /* cancel and join the philosopher threads */
    for (i = 0; i < PHILNUM; i++)
        pth_cancel((tab->tid)[i]);
    while (pth_join(NULL, NULL));

    /* finish display */
    printf("+----------+----------+----------+----------+----------+\n");

    /* free the control table */
    free(tab);

    /* shutdown Pth library */
    pth_kill();

    return 0;
}
