#include <cstdio>
#include <stdexcept>

static int check(int n) {
    if (n > 2) throw std::runtime_error("too many");
    return n;
}

int main() {
    int total = 0;
    for (int i = 0; i < 5; ++i) total += check(i);
    std::printf("%d\n", total);
    return 0;
}
