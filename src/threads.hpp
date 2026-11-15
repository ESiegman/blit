#pragma once
// Work spread over several threads at once, for code that would rather not
// take in <thread> (a costly header) for it.
#include <functional>

namespace blit {

// How many threads the machine runs at once (at least 1).
unsigned cores();

// Runs `work` on `count` threads at once, this one among them, and returns
// when every one of them has.
void on_threads(unsigned count, std::function_ref<void()> work);

}  // namespace blit
