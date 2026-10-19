// A51 probe #2: async IO accounting (submit vs completion). Gated by APS5_TRACE_IO=1.
// Counted in: sceKernelAio* submit/wait (Aio.cpp) and libSceAmpr/Ampr submit (Export.cpp).
// At 60 s a single [async] summary line is printed: submits vs completions per API,
// last activity age, and totals. "Submits without completions" = a blocked reader.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace Aps5Async {

inline std::atomic<long long> gAioSubmit{0};
inline std::atomic<long long> gAioComplete{0};
inline std::atomic<long long> gAmprSubmit{0};
inline std::atomic<long long> gAmprBytes{0};
inline std::atomic<long long> gAmprWait{0};
inline std::atomic<long long> gLastActivityMs{0};
inline std::atomic<bool> gPrinted{false};
inline std::atomic<bool> gWatcherStarted{false};
inline std::mutex gPrintMutex;
inline std::chrono::steady_clock::time_point gStart = std::chrono::steady_clock::now();

inline long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void summary(const char* tag);

inline void maybeWatch() {
    const char* v = std::getenv("APS5_TRACE_IO");
    if (!(v && v[0] != '0' && v[0] != '\0'))
        return;
    if (gWatcherStarted.exchange(true))
        return;
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - gStart).count();
            if (sec >= 60.0) {
                summary("A51@60s");
                return;
            }
        }
    }).detach();
}

inline void summary(const char* tag) {
    if (gPrinted.exchange(true))
        return;
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - gStart).count();
    long long idleMs = nowMs() - gLastActivityMs.load();
    char line[300];
    std::snprintf(line, sizeof(line),
                  "[async] summary @%.0fs (%s): aio submit=%lld complete=%lld | ampr submit=%lld bytes=%lld waits=%lld | idle since last async op: %lld ms\n",
                  sec, tag ? tag : "",
                  (long long)gAioSubmit.load(), (long long)gAioComplete.load(),
                  (long long)gAmprSubmit.load(), (long long)gAmprBytes.load(),
                  (long long)gAmprWait.load(), (long long)idleMs);
    std::lock_guard<std::mutex> lock(gPrintMutex);
    std::fwrite(line, 1, std::strlen(line), stderr);
    std::fflush(stderr);
}

inline void mark() {
    gLastActivityMs.store(nowMs());
}

inline void aioSubmit(long n) {
    gAioSubmit += n;
    mark();
    maybeWatch();
}
inline void aioComplete(long n) {
    gAioComplete += n;
    mark();
}
inline void amprSubmit(long bytes) {
    gAmprSubmit += 1;
    gAmprBytes += bytes;
    mark();
    maybeWatch();
}
inline void amprWait() {
    gAmprWait += 1;
    mark();
}

} // namespace Aps5Async
