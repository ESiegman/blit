#include "b.hpp"
template <class T> int f(T t) { return t.nope; }
int main() {
    int unused = 3;
    return f(2) + undeclared;
}
