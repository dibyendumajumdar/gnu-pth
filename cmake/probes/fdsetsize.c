/* Probe: size of fd_set (mirrors AC_CHECK_FDSETSIZE). Prints the value. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#include <sys/time.h>

int main(int argc, char *argv[])
{
#if defined(FD_SETSIZE)
    printf("%d\n", FD_SETSIZE);
#else
    printf("%d\n", 1024);
#endif
    exit(0);
}
