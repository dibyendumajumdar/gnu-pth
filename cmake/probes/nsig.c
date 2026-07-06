/* Probe: number of signals (mirrors AC_CHECK_NSIG). Prints the value. */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

int main(int argc, char *argv[])
{
#if defined(NSIG)
    printf("%d\n", NSIG);
#elif defined(_NSIG)
    printf("%d\n", _NSIG);
#else
    printf("%d\n", 32);
#endif
    exit(0);
}
