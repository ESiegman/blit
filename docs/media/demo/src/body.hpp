#pragma once
#include <string>
#include <vector>

// A body in the plane: where it is, how fast it moves, how heavy it is.
struct Vec2 {
    double x = 0, y = 0;
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(double k) const { return {x * k, y * k}; }
    double length() const;
};

struct Body {
    std::string name;
    Vec2 position;
    Vec2 velocity;
    double mass = 1;
};

// One step of the simulation: every body pulls on every other.
void step(std::vector<Body>& bodies, double dt, double gravity);
double energy(const std::vector<Body>& bodies, double gravity);
