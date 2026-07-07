/* Cross-scheduler message ports: a receiver on scheduler 0 waits on a port
   while senders on other schedulers pth_msgport_put() into it. Verifies
   every message is delivered (count + checksum) with no lost-wakeup hang,
   exercising the spinlock protection and the put()-side scheduler kick. */
#include "pth.h"
#include <stdio.h>
#include <stdlib.h>

#define NSCHED 4
#define NSEND  3            /* senders on schedulers 1..3 */
#define NPER   500
#define NTOTAL (NSEND*NPER)

static pth_msgport_t port;

static void *sender(void *arg)
{
    long base = (long)arg;      /* 1..NSEND */
    int i;
    for (i = 0; i < NPER; i++) {
        pth_message_t *m = (pth_message_t *)malloc(sizeof(pth_message_t));
        m->m_replyport = NULL;
        m->m_size      = 0;
        m->m_data      = (void *)(base*1000 + i);
        pth_msgport_put(port, m);
        if ((i & 63) == 0)
            pth_yield(NULL);    /* interleave so the receiver really blocks */
    }
    return NULL;
}

int main(void)
{
    pth_attr_t attr;
    static pth_key_t evk = PTH_KEY_INIT;
    long got = 0, sum = 0, expect_sum = 0;
    int s, i, fails = 0;

    pth_init();
    if (!pth_mp_init(NSCHED)) { fprintf(stderr, "no PTH_MP\n"); return 1; }
    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, FALSE);

    port = pth_msgport_create("test-port");

    for (s = 1; s <= NSEND; s++) {
        if (pth_spawn_on(s, attr, sender, (void *)(long)s) == NULL) {
            fprintf(stderr, "spawn_on(%d) failed\n", s); return 1;
        }
        for (i = 0; i < NPER; i++)
            expect_sum += s*1000 + i;
    }

    /* receive loop: block on the port, then drain everything available */
    while (got < NTOTAL) {
        pth_event_t ev = pth_event(PTH_EVENT_MSG|PTH_MODE_STATIC, &evk, port);
        pth_message_t *m;
        pth_wait(ev);
        while ((m = pth_msgport_get(port)) != NULL) {
            got++;
            sum += (long)m->m_data;
            free(m);
        }
    }

    pth_msgport_destroy(port);
    pth_attr_destroy(attr);
    pth_mp_shutdown();

    printf("received : %ld (want %d) %s\n", got, NTOTAL, got==NTOTAL ? "OK":"FAIL");
    printf("checksum : %ld (want %ld) %s\n", sum, expect_sum, sum==expect_sum ? "OK":"FAIL");
    if (got != NTOTAL) fails++;
    if (sum != expect_sum) fails++;
    printf(fails==0 ? "ALL MSGPORT-MP TESTS PASSED\n" : "%d MSGPORT-MP TESTS FAILED\n", fails);
    return fails==0 ? 0 : 1;
}
