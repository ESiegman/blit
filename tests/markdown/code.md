# Code in colour

Blocks in a language there is a grammar for are coloured as the editor colours them.

```cpp
// a comment
#include <cstdio>
int main() { return std::puts("hi") > 0 ? 0 : 1; }
```

```c++
auto x = 1.5f;
```

```py
def f(x):
	return "s" + str(x)  # a tab before it
```

```sh
for f in *.md; do echo "$f"; done
```

```json
{"a": [1, true, null]}
```

```toml
[table]
key = "value"
```

```yaml
list:
  - one
```

```cmake
add_executable(blit main.cpp)
```

```lua
local t = { 1, 2 } -- a comment
```

```nosuchlang
int plain; // left as it is
```

```
no language at all
```

```cpp
const char* s = "a long line that has to wrap round, because it is wider than the preview is, which is narrow";
```
