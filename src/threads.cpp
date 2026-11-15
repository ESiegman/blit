#include "threads.hpp"

#include <thread>
#include <vector>

namespace blit {

unsigned cores() {
    const unsigned n = std::thread::hardware_concurrency();
    return n ? n : 1;
}

void on_threads(unsigned count, std::function_ref<void()> work) {
    std::vector<std::jthread> helpers;
    for (unsigned t = 1; t < count; ++t) helpers.emplace_back([&] { work(); });
    work();
}

}  // namespace blit
