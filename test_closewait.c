/*
**  test_closewait.c -- a descriptor closed while a Pth thread is still blocked
**  waiting on it must not strand that thread.  On select/poll the closed fd
**  surfaces as EBADF immediately; on the persistent epoll/kqueue backends the
**  bounded-revalidation heartbeat (PTH_SCHED_REVAL_MS) re-asserts registrations
**  and wakes the waiter with an error within one interval.  Without that safety
**  net this test HANGS on the epoll/kqueue backends (caught by the harness
**  timeout).  Backend-agnostic; single scheduler is enough.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>

#include "pth.h"
#include "test_common.h"

static int sv[2];

static void *reader(void *arg)
{
    char b;
    ssize_t n = pth_read(sv[0], &b, 1);      /* blocks: no data, no timeout */
    (void)arg;
    return (void *)(long)(n < 0 ? -errno : (int)n);
}

int main(void)
{
    pth_t th;
    pth_attr_t attr;
    void *r = NULL;
    long rc;

    /* short revalidation interval so the close is noticed quickly */
    setenv("PTH_SCHED_REVAL_MS", "200", 1);
    pth_init();

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair"); pth_kill(); return 1;
    }

    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);
    th = pth_spawn(attr, reader, NULL);
    pth_attr_destroy(attr);

    pth_nap(pth_time(0, 150000));   /* 150ms: let the reader block in pth_read */
    close(sv[0]);                   /* close the very fd it is waiting on
                                       (sv[1] stays open, so this is a bare
                                       close-while-waiting, not an EOF) */

    /* must return rather than hang; the harness timeout is the backstop */
    pth_join(th, &r);
    close(sv[1]);
    rc = (long)r;

    printf("reader woke: pth_read -> %ld (%s)\n", rc,
           rc < 0 ? "error: did not strand (correct)"
                  : "readable/EOF");

    pth_kill();

    /* the essential property is liveness: the waiter woke at all. An error is
       the expected result of closing a descriptor out from under it. */
    if (rc > 0) {
        printf("unexpected readable result -- FAIL\n");
        return 1;
    }
    printf("ALL CLOSEWAIT TESTS PASSED\n");
    return 0;
}
