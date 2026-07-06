/* Probe: direction of stack growth (mirrors AC_CHECK_STACKGROWTH).
   Prints "down" or "up". */
#include <stdio.h>
#include <stdlib.h>

static int iterate = 10;

static int growsdown(int *x)
{
    int y;
    y = (x > &y);
    if (--iterate > 0)
        y = growsdown(&y);
    if (y != (x > &y))
        exit(1);
    return y;
}

int main(int argc, char *argv[])
{
    int x;
    printf("%s\n", growsdown(&x) ? "down" : "up");
    exit(0);
}
