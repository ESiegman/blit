// A component the project fetched (tests/crash.sh puts it under
// managed_components): code that is not the project's own.
__attribute__((noinline)) void ring_put(volatile int *slot, int v)
{
    *slot = v;
}
