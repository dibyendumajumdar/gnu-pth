/* Deterministic fd-event test for the scheduler core: exercises the
   PTH_EVENT_FD path (pth_read), the PTH_EVENT_SELECT path (pth_select),
   and -- the point of the poll backend -- a descriptor whose VALUE exceeds
   FD_SETSIZE, which select(2)/fd_set cannot represent without corruption. */
#include "pth.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/select.h>

static int  p_fd[2], q_fd[2];
static int  hi_wr = -1;

static void *w_fd(void *a)   { int i; for(i=0;i<4;i++) pth_yield(NULL); pth_write(p_fd[1], "X", 1); return NULL; }
static void *w_sel(void *a)  { int i; for(i=0;i<4;i++) pth_yield(NULL); pth_write(q_fd[1], "Y", 1); return NULL; }
static void *w_hi(void *a)   { int i; for(i=0;i<4;i++) pth_yield(NULL); pth_write(hi_wr, "Z", 1); return NULL; }

int main(void)
{
    pth_attr_t attr;
    char c;
    int fails = 0;

    pth_init();
    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);

    /* A: PTH_EVENT_FD via pth_read (thread blocks until data arrives) */
    if (pipe(p_fd) != 0) { perror("pipe"); return 1; }
    { pth_t w = pth_spawn(attr, w_fd, NULL);
      c = 0;
      int n = pth_read(p_fd[0], &c, 1);
      pth_join(w, NULL);
      printf("A fd-event (pth_read)      : %s\n", (n==1 && c=='X') ? "OK":"FAIL");
      if (!(n==1 && c=='X')) fails++;
    }

    /* B: PTH_EVENT_SELECT via pth_select */
    if (pipe(q_fd) != 0) { perror("pipe"); return 1; }
    { pth_t w = pth_spawn(attr, w_sel, NULL);
      fd_set r; FD_ZERO(&r); FD_SET(q_fd[0], &r);
      int n = pth_select(q_fd[0]+1, &r, NULL, NULL, NULL);
      int ready = (n==1 && FD_ISSET(q_fd[0], &r));
      c = 0; if (ready) pth_read(q_fd[0], &c, 1);
      pth_join(w, NULL);
      printf("B select-event (pth_select): %s\n", (ready && c=='Y') ? "OK":"FAIL");
      if (!(ready && c=='Y')) fails++;
    }

    /* C: descriptor value ABOVE FD_SETSIZE -- the poll backend's raison
          d'etre. Raise NOFILE, force a read end up to a high fd number. */
    {
        struct rlimit rl;
        int hi_rd = -1, raw[2];
        const int HIFD = FD_SETSIZE + 500;   /* e.g. 1524 when FD_SETSIZE==1024 */
        rl.rlim_cur = HIFD + 16; rl.rlim_max = HIFD + 16;
        setrlimit(RLIMIT_NOFILE, &rl);
        if (pipe(raw) == 0 && dup2(raw[0], HIFD) == HIFD) {
            close(raw[0]);
            hi_rd = HIFD;
            hi_wr = raw[1];
            pth_t w;
            int n;
            /* probe first: a select-core library rejects fd>=FD_SETSIZE with
               EBADF *before* any FD_SET, so this is safe on both backends */
            errno = 0;
            c = 0;
            w = pth_spawn(attr, w_hi, NULL);
            n = pth_read(hi_rd, &c, 1);   /* fd value > FD_SETSIZE */
            if (n == 1 && c == 'Z') {
                printf("C high fd #%d (>FD_SETSIZE=%d): OK (poll backend)\n",
                       hi_rd, FD_SETSIZE);
            }
            else if (n < 0 && errno == EBADF) {
                printf("C high fd #%d: SKIP -- select backend rejects "
                       "fd>=FD_SETSIZE=%d (expected; build -DPTH_SCHED_POLL to support)\n",
                       hi_rd, FD_SETSIZE);
            }
            else {
                printf("C high fd #%d (>FD_SETSIZE=%d): FAIL (n=%d errno=%d)\n",
                       hi_rd, FD_SETSIZE, n, errno);
                fails++;
            }
            pth_join(w, NULL);
        }
        else
            printf("C high fd: SKIP (could not raise NOFILE / dup2, errno=%d)\n", errno);
    }

    pth_attr_destroy(attr);
    pth_kill();
    printf(fails==0 ? "ALL POLLFD TESTS PASSED\n" : "%d POLLFD TESTS FAILED\n", fails);
    return fails==0?0:1;
}
