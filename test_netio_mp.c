/* Cross-scheduler network I/O under PTH_MP: the two endpoints of each data
   flow are pinned to *different* schedulers, so this exercises each scheduler's
   own select(2)/poll(2) event loop reacting to fd readiness driven by a thread
   running in parallel on another scheduler -- the part of the API the other MP
   tests do not touch.

   Part 1: N socketpair streams. Producers push a verifiable byte pattern with
           pth_write / pth_send; consumers drain it with pth_read / pth_select+
           pth_read, checking every byte (catches loss, corruption, reorder).
           Socket buffers are shrunk so the transfer blocks repeatedly in both
           directions, forcing the cross-scheduler fd-wakeup path.
   Part 2: TCP loopback. A listener thread (one scheduler) pth_accept()s a
           connection a connector thread (another scheduler) pth_connect()s,
           then they exchange a message. */
#include "pth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define NSCHED 4
#define NPAIRS 4
#define MSGSZ  4096
#define NMSG   64
#define TOTAL  ((long)MSGSZ*NMSG)

static int  sp[NPAIRS][2];
static long recv_bytes[NPAIRS];
static int  pair_err[NPAIRS];

static unsigned char patt(int pair, long idx) { return (unsigned char)((pair*131 + idx) & 0xFF); }

static void small_bufs(int fd) { int v = 4096; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v));
                                 setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v)); }

static void *producer(void *arg)
{
    long p = (long)arg, sent = 0; int fd = sp[p][0], m;
    unsigned char buf[MSGSZ];
    for (m = 0; m < NMSG; m++) {
        long off, i;
        for (i = 0; i < MSGSZ; i++) buf[i] = patt((int)p, sent + i);
        for (off = 0; off < MSGSZ; ) {
            ssize_t w = (p & 1) ? pth_send(fd, buf+off, MSGSZ-off, 0)
                                : pth_write(fd, buf+off, MSGSZ-off);
            if (w <= 0) { pair_err[p] = 1; return NULL; }
            off += w;
        }
        sent += MSGSZ;
    }
    shutdown(fd, SHUT_WR);      /* signal EOF to the consumer on its scheduler */
    return NULL;
}

static void *consumer(void *arg)
{
    long p = (long)arg, got = 0; int fd = sp[p][1];
    unsigned char buf[2000];
    for (;;) {
        ssize_t r; long i;
        if (p & 1) {                                   /* PTH_EVENT_SELECT path */
            fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf);
            int n = pth_select(fd+1, &rf, NULL, NULL, NULL);
            if (n < 0) { pair_err[p] = 1; break; }
        }
        r = pth_read(fd, buf, sizeof(buf));            /* PTH_EVENT_FD path */
        if (r == 0) break;                             /* EOF */
        if (r < 0) { pair_err[p] = 1; break; }
        for (i = 0; i < r; i++)
            if (buf[i] != patt((int)p, got + i)) { pair_err[p] = 1; break; }
        if (pair_err[p]) break;
        got += r;
    }
    recv_bytes[p] = got;
    return NULL;
}

/* ---- Part 2: cross-scheduler connect/accept ---- */
static int  listen_fd, tcp_ok;
static void *acceptor(void *arg)
{
    int c = pth_accept(listen_fd, NULL, NULL);
    char b[8] = {0};
    if (c < 0) return NULL;
    if (pth_read(c, b, 4) == 4 && memcmp(b, "ping", 4) == 0)
        if (pth_write(c, "pong", 4) == 4) tcp_ok = 1;
    close(c);
    return NULL;
}
static void *connector(void *arg)
{
    struct sockaddr_in *sa = (struct sockaddr_in *)arg;
    char b[8] = {0};
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return NULL;
    if (pth_connect(s, (struct sockaddr *)sa, sizeof(*sa)) == 0)
        if (pth_write(s, "ping", 4) == 4)
            pth_read(s, b, 4);                          /* expect "pong" */
    close(s);
    return NULL;
}

int main(void)
{
    pth_attr_t attr;
    pth_t prod[NPAIRS], cons[NPAIRS], la, lc;
    struct sockaddr_in sa;
    socklen_t slen = sizeof(sa);
    int ns, p, fails = 0;

    pth_init();
    if (!pth_mp_init(NSCHED)) { fprintf(stderr, "no PTH_MP\n"); return 1; }
    ns = pth_sched_count();
    attr = pth_attr_new();
    pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);

    /* ---- Part 1 ---- */
    for (p = 0; p < NPAIRS; p++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp[p]) != 0) { perror("socketpair"); return 1; }
        small_bufs(sp[p][0]); small_bufs(sp[p][1]);
    }
    for (p = 0; p < NPAIRS; p++) {
        prod[p] = pth_spawn_on((2*p)   % ns, attr, producer, (void *)(long)p);
        cons[p] = pth_spawn_on((2*p+1) % ns, attr, consumer, (void *)(long)p);
    }
    for (p = 0; p < NPAIRS; p++) { pth_join(prod[p], NULL); pth_join(cons[p], NULL); }

    for (p = 0; p < NPAIRS; p++) {
        int ok = (!pair_err[p] && recv_bytes[p] == TOTAL);
        if (!ok) fails++;
        close(sp[p][0]); close(sp[p][1]);
    }
    printf("part1 socketpair streams: %d pairs x %ld bytes (write/send + read/select), %s\n",
           NPAIRS, TOTAL, fails == 0 ? "OK" : "FAIL");

    /* ---- Part 2 ---- */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = 0;
    { int one=1; setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)); }
    if (bind(listen_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(listen_fd, 1) != 0 ||
        getsockname(listen_fd, (struct sockaddr *)&sa, &slen) != 0) {
        printf("part2 connect/accept: SKIP (loopback bind failed, errno=%d)\n", errno);
    } else {
        la = pth_spawn_on(1 % ns, attr, acceptor,  NULL);
        lc = pth_spawn_on(2 % ns, attr, connector, &sa);
        pth_join(la, NULL); pth_join(lc, NULL);
        close(listen_fd);
        printf("part2 connect/accept (cross-scheduler): %s\n", tcp_ok ? "OK" : "FAIL");
        if (!tcp_ok) fails++;
    }

    pth_attr_destroy(attr);
    pth_mp_shutdown();
    printf("schedulers: %d\n", ns);
    printf(fails == 0 ? "ALL NETIO-MP TESTS PASSED\n" : "%d NETIO-MP TESTS FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
