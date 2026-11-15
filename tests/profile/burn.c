#include <stdio.h>
#include <stdlib.h>
static double leaf_a(int n) { volatile double s = 0; for (int i = 0; i < n; ++i) s += i * 0.5; return s; }
static double leaf_b(int n) { volatile double s = 1; for (int i = 1; i < n; ++i) s *= 1.0000001; return s; }
__attribute__((noinline)) double middle(int n) { return leaf_a(n) + leaf_b(n / 2); }
__attribute__((noinline)) double top(int r) { double t = 0; for (int i = 0; i < r; ++i) t += middle(2000000); return t; }
int main(int argc, char** argv) { int r = argc > 1 ? atoi(argv[1]) : 300; printf("%f\n", top(r)); return 0; }
