#include "threads.hpp"

#include <memory>
#include <cstdlib>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <pthread.h>
#endif

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

#if defined(_WIN32)
DeepThread::DeepThread(size_t, std::function<void()> work) {
    handle_ = reinterpret_cast<unsigned long long>(new std::thread(std::move(work)));
    live_ = true;
}

void DeepThread::join() {
    if (!live_) return;
    auto* t = reinterpret_cast<std::thread*>(handle_);
    t->join();
    delete t;
    live_ = false;
}
#else
DeepThread::DeepThread(size_t stack_bytes, std::function<void()> work) {
    auto run = [](void* p) -> void* {
        std::unique_ptr<std::function<void()>> w(static_cast<std::function<void()>*>(p));
        (*w)();
        return nullptr;
    };
    auto* w = new std::function<void()>(std::move(work));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_bytes);
    pthread_t t;
    int err = pthread_create(&t, &attr, run, w);
    pthread_attr_destroy(&attr);
    if (err) err = pthread_create(&t, nullptr, run, w);  // (a stack that large refused: the system's)
    if (err) {
        delete w;
        std::abort();  // (as std::thread would end blit)
    }
    static_assert(sizeof(pthread_t) <= sizeof(handle_));
    handle_ = 0;
    __builtin_memcpy(&handle_, &t, sizeof t);
    live_ = true;
}

void DeepThread::join() {
    if (!live_) return;
    pthread_t t;
    __builtin_memcpy(&t, &handle_, sizeof t);
    pthread_join(t, nullptr);
    live_ = false;
}
#endif

DeepThread::DeepThread(DeepThread&& o) noexcept : handle_(std::exchange(o.handle_, 0)), live_(std::exchange(o.live_, false)) {}

DeepThread& DeepThread::operator=(DeepThread&& o) noexcept {
    if (this != &o) {
        join();
        handle_ = std::exchange(o.handle_, 0);
        live_ = std::exchange(o.live_, false);
    }
    return *this;
}

DeepThread::~DeepThread() { join(); }

}  // namespace blit
