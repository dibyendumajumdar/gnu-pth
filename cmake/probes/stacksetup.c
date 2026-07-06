/* Probe: how to specify stacks (mirrors AC_CHECK_STACKSETUP).
   Compile with exactly one of: -DTEST_makecontext, -DTEST_sigaltstack,
   -DTEST_sigstack (and -DHAVE_STACK_T if stack_t exists).
   Prints "<skaddr-expr>,<sksize-expr>", e.g. "(skaddr),(sksize)". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#if defined(TEST_makecontext)
#include <ucontext.h>
#endif

union alltypes {
    long   l;
    double d;
    void  *vp;
    void (*fp)(void);
    char  *cp;
};

static volatile char *handler_addr = (char *)0xDEAD;
#if defined(TEST_makecontext)
static ucontext_t uc_handler;
static ucontext_t uc_main;
#else
static volatile int handler_done = 0;
#endif

static void handler(void)
{
    char garbage[1024];
    int i;
    auto char stack_probe;
    for (i = 0; i < 1024; i++)
        garbage[i] = 'X';
    handler_addr = &stack_probe;
#if defined(TEST_makecontext)
    swapcontext(&uc_handler, &uc_main);
#else
    handler_done = 1;
#endif
    return;
}

#if !defined(TEST_makecontext)
static void handler_sig(int sig)
{
    handler();
}
#endif

int main(int argc, char *argv[])
{
    char *skaddr;
    char *skbuf;
    int sksize;
    char result[1024];
    int i;

    sksize = 32768;
    skbuf = (char *)malloc(sksize*2+2*sizeof(union alltypes));
    if (skbuf == NULL)
        exit(1);
    for (i = 0; i < (int)(sksize*2+2*sizeof(union alltypes)); i++)
        skbuf[i] = 'A';
    skaddr = skbuf+sizeof(union alltypes);

#if defined(TEST_sigstack) || defined(TEST_sigaltstack)
    {
        struct sigaction sa;
#if defined(TEST_sigstack)
        struct sigstack ss;
#elif defined(TEST_sigaltstack) && defined(HAVE_STACK_T)
        stack_t ss;
#else
        struct sigaltstack ss;
#endif
#if defined(TEST_sigstack)
        ss.ss_sp      = (void *)(skaddr + sksize);
        ss.ss_onstack = 0;
        if (sigstack(&ss, NULL) < 0)
            exit(1);
#elif defined(TEST_sigaltstack)
        ss.ss_sp    = (void *)(skaddr + sksize);
        ss.ss_size  = sksize;
        ss.ss_flags = 0;
        if (sigaltstack(&ss, NULL) < 0)
            exit(1);
#endif
        memset((void *)&sa, 0, sizeof(struct sigaction));
        sa.sa_handler = handler_sig;
        sa.sa_flags = SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);
        kill(getpid(), SIGUSR1);
        while (!handler_done)
            /*nop*/;
    }
#endif
#if defined(TEST_makecontext)
    {
        if (getcontext(&uc_handler) != 0)
            exit(1);
        uc_handler.uc_link = NULL;
        uc_handler.uc_stack.ss_sp    = (void *)(skaddr + sksize);
        uc_handler.uc_stack.ss_size  = sksize;
        uc_handler.uc_stack.ss_flags = 0;
        makecontext(&uc_handler, handler, 1);
        swapcontext(&uc_main, &uc_handler);
    }
#endif

    if (handler_addr == (char *)0xDEAD)
        exit(1);
    if (handler_addr < skaddr+sksize) {
        /* stack was placed into lower area */
        if (*(skaddr+sksize) != 'A')
            sprintf(result, "(skaddr)+(sksize)-%d,(sksize)-%d",
                    (int)sizeof(union alltypes), (int)sizeof(union alltypes));
        else
            strcpy(result, "(skaddr)+(sksize),(sksize)");
    }
    else {
        /* stack was placed into higher area */
        if (*(skaddr+sksize*2) != 'A')
            sprintf(result, "(skaddr),(sksize)-%d", (int)sizeof(union alltypes));
        else
            strcpy(result, "(skaddr),(sksize)");
    }
    printf("%s\n", result);
    exit(0);
}
