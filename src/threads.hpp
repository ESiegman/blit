#pragma once
// Work spread over several threads at once, for code that would rather not
// take in <thread> (a costly header) for it.
#include <cstddef>
#include <functional>

namespace blit {

// How many threads the machine runs at once (at least 1).
unsigned cores();

// Runs `work` on `count` threads at once, this one among them, and returns
// when every one of them has.
void on_threads(unsigned count, std::function_ref<void()> work);

// A thread with a stack as large as asked: a thread made by std::thread has
// the system's (512 KB on macOS), which work walking deep trees runs out of.
// Joined, if it still runs, when it goes.
class DeepThread {
public:
    DeepThread() = default;
    DeepThread(size_t stack_bytes, std::function<void()> work);
    DeepThread(DeepThread&& o) noexcept;
    DeepThread& operator=(DeepThread&& o) noexcept;
    ~DeepThread();
    bool joinable() const { return live_; }
    void join();

private:
    unsigned long long handle_ = 0;
    bool live_ = false;
};

}  // namespace blit
