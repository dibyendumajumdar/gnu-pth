/* Probe: usable SVR4/SUSv2 makecontext(2)/swapcontext(2)
   (exit 0 = usable) */
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>

static ucontext_t uc_child;
static ucontext_t uc_main;

static void child(void *arg)
{
    if (arg != (void *)12345)
        exit(1);
    if (swapcontext(&uc_child, &uc_main) != 0)
        exit(1);
}

int main(int argc, char *argv[])
{
    void *stack;

    if ((stack = malloc(64*1024)) == NULL)
        exit(1);
    if (getcontext(&uc_child) != 0)
        exit(1);
    uc_child.uc_link = NULL;
    uc_child.uc_stack.ss_sp = (char *)stack+(32*1024);
    uc_child.uc_stack.ss_size = 32*1024;
    uc_child.uc_stack.ss_flags = 0;
    makecontext(&uc_child, (void (*)(void))child, 1, (void *)12345);
    if (swapcontext(&uc_main, &uc_child) != 0)
        exit(1);
    exit(0);
}
