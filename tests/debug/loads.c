// A program for the debugging tests that loads a library as it runs
// (twice.c, built beside it as loads.so): a breakpoint in the library is
// not verified until it is loaded.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv)
{
    (void)argc;
    char* lib = malloc(strlen(argv[0]) + 4);
    sprintf(lib, "%s.so", argv[0]);
    void* h = dlopen(lib, RTLD_NOW);
    int (*twice)(int) = h ? (int (*)(int))dlsym(h, "twice") : NULL;
    printf("%d\n", twice ? twice(21) : -1);
    free(lib);
    return 0;
}
