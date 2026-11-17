# orbits

A few bodies pulling on one another, stepped with semi-implicit Euler.

![blit's icon](logo.png)

| Body | Mass | Distance |
|------|-----:|---------:|
| sun | 1000 | 0 |
| earth | 1 | 1.00 |
| moon | 0.01 | 1.05 |

```mermaid
flowchart TD
    A[read bodies] --> B[pull on each other]
    B --> C[move]
    C --> D{steps left?}
    D -- yes --> B
    D -- no --> E[print energy drift]
```

```cpp
for (int i = 0; i < steps; ++i) step(bodies, dt, gravity);
```

- [x] gravity
- [ ] collisions
- [ ] a picture of the orbits
