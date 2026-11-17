#include "body.hpp"

#include <cmath>

double Vec2::length() const { return std::sqrt(x * x + y * y); }

void step(std::vector<Body>& bodies, double dt, double gravity) {
    std::vector<Vec2> pull(bodies.size());
    for (size_t i = 0; i < bodies.size(); ++i)
        for (size_t j = 0; j < bodies.size(); ++j) {
            if (i == j) continue;
            const Vec2 d = bodies[j].position - bodies[i].position;
            const double r = std::max(d.length(), 1e-3);
            pull[i] = pull[i] + d * (gravity * bodies[j].mass / (r * r * r));
        }
    for (size_t i = 0; i < bodies.size(); ++i) {
        bodies[i].velocity = bodies[i].velocity + pull[i] * dt;
        bodies[i].position = bodies[i].position + bodies[i].velocity * dt;
    }
}

double energy(const std::vector<Body>& bodies, double gravity) {
    double kinetic = 0, potential = 0;
    for (size_t i = 0; i < bodies.size(); ++i) {
        const Body& b = bodies[i];
        kinetic += 0.5 * b.mass * (b.velocity.x * b.velocity.x + b.velocity.y * b.velocity.y);
        for (size_t j = i + 1; j < bodies.size(); ++j)
            potential -= gravity * b.mass * bodies[j].mass / (bodies[j].position - b.position).length();
    }
    return kinetic + potential;
}
