#include <stdlib.h>

int pick(int k) {
    int *p = malloc(sizeof(int));
    if (k > 2)
        return 0;
    *p = k;
    int v = *p;
    free(p);
    return v;
}

int divide(int k) {
    int z = 0;
    if (k)
        z = 1;
    return k / z;
}
