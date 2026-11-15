#pragma once
// How soon a thread's work is wanted, told to the system's scheduler: on
// macOS a "quality of service", which decides which cores a thread may run
// on and how it fares against the thread that draws (which keeps the
// highest). Without it a worker has its maker's: the drawing thread's.
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace blit {

enum class Soon {
    kWaitedFor,   // somebody is waiting for the answer: colours after an edit, a search, a completion
    kBackground,  // nobody is: git's marks, a measurement, a grammar being built
};

inline void thread_soon(Soon s) {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(s == Soon::kWaitedFor ? QOS_CLASS_USER_INITIATED : QOS_CLASS_UTILITY, 0);
#else
    (void)s;
#endif
}

}  // namespace blit
