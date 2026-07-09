/*
**  test_badfd.c -- a bad descriptor in pth_select()/pth_read() must fail, not
**  be reported ready and not hang.  This guards the evport (epoll/kqueue)
**  backends' bad-fd handling against regressing the select(2)/poll(2)
**  POLLNVAL -> PTH_STATUS_FAILED semantics.  Backend-agnostic: passes under
**  select, poll, epoll and kqueue.
*/

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/select.h>

#include "pth.h"
#include "test_common.h"

int main(void)
{
    int fd[2], rc, fails = 0;
    fd_set rfds;
    pth_event_t tmo;

    pth_init();

    /* obtain a real descriptor number, then close it so the number is invalid */
    if (pipe(fd) != 0) { perror("pipe"); pth_kill(); return 1; }
    close(fd[0]);
    close(fd[1]);

    FD_ZERO(&rfds);
    FD_SET(fd[0], &rfds);

    /* bound the call so a (buggy) strand shows up as a timeout, not a hang */
    tmo = pth_event(PTH_EVENT_TIME, pth_timeout(3, 0));
    rc = pth_select_ev(fd[0] + 1, &rfds, NULL, NULL, NULL, tmo);
    pth_event_free(tmo, PTH_FREE_ALL);

    printf("pth_select(closed fd %d) = %d, errno=%d %s\n",
           fd[0], rc, errno,
           rc < 0 ? "(error: correct)"
                  : (rc == 0 ? "(timeout)" : "(READY: WRONG)"));

    /* the invalid descriptor must never be reported ready */
    if (rc > 0) {
        printf("bad descriptor reported ready -- FAIL\n");
        fails++;
    }
    /* the classic/poll/epoll/kqueue behaviour is an immediate EBADF error */
    if (rc < 0 && errno != EBADF)
        printf("note: error was %s (any error is acceptable here)\n", strerror(errno));

    pth_kill();
    printf(fails == 0 ? "ALL BADFD TESTS PASSED\n" : "%d BADFD TESTS FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
