#ifndef ROBOTWEAX_SRT_TELEMETRY_THREAD_NAME_H
#define ROBOTWEAX_SRT_TELEMETRY_THREAD_NAME_H

#include <cstddef>

#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

namespace robotweax_srt_telemetry_internal
{
/* Names the calling thread, so it does not show the name of the host thread
 * that started it (threads inherit it on Linux). At most 15 characters, the
 * Linux limit (a longer name would silently not be set). Best effort; other
 * platforms keep their default names. */
template <std::size_t N> inline void name_current_thread(const char (&name)[N])
{
    static_assert(N <= 16, "thread names have at most 15 characters");
#if defined(__linux__)
    static_cast<void>(pthread_setname_np(pthread_self(), name));
#elif defined(__APPLE__)
    static_cast<void>(pthread_setname_np(name));
#else
    static_cast<void>(name);
#endif
}
} // namespace robotweax_srt_telemetry_internal

#endif
