#include <cstdio>
#include <cstdlib>
#include <vector>

#include "body.hpp"

int main(int argc, char** argv) {
    const int steps = argc > 1 ? std::atoi(argv[1]) : 1000;
    const double gravity = 1.0, dt = 0.001;
    std::vector<Body> bodies = {
        {"sun", {0, 0}, {0, 0}, 1000},
        {"earth", {1, 0}, {0, 31.6}, 1},
        {"moon", {1.05, 0}, {0, 36.1}, 0.01},
    };
    const double before = energy(bodies, gravity);
    for (int i = 0; i < steps; ++i) step(bodies, dt, gravity);
    const double after = energy(bodies, gravity);
    for (const Body& b : bodies)
        std::printf("%-6s %8.3f %8.3f\n", b.name.c_str(), b.position.x, b.position.y);
    std::printf("energy drift: %.2e\n", (after - before) / before);
}
