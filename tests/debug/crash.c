// A program for the debugging tests' core files: it crashes in a function
// it calls, with a value to read.
#include <stdlib.h>

static int fall(int depth)
{
    int here = depth * 7;
    if (depth == 3) abort();
    return fall(depth + 1) + here;
}

int main(void)
{
    return fall(1);
}
