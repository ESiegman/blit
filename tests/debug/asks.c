#include <stdio.h>
// A program that reads what is typed to it: tests/debug.sh gives it a terminal.
int main(void) {
    char name[64] = "";
    printf("name? ");
    fflush(stdout);
    if (!fgets(name, sizeof name, stdin)) return 1;
    printf("hello, %s", name);
    return 0;
}
