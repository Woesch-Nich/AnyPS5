#pragma once

// Request = kernel open / APR file-id resolve (the engine asking for a file);
// completion = the read actually delivered bytes (or the error it returned).
// A request that is "completed" without a matching completion line is our bug.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace Aps5RequestLog {

inline bool enabled() {
    static const bool on = std::getenv("APS5_TRACE_REQ") != nullptr;
    return on;
}

inline void emit(const char* fmt, ...) {
    if (!enabled()) return;
    static std::mutex lock;
    std::lock_guard guard(lock);
    va_list a;
    va_start(a, fmt);
    std::vfprintf(stderr, fmt, a);
    va_end(a);
    std::fflush(stderr);
}

// guestPath: what the engine asked for; nativePath: what we mapped it to
// (they differ when the path does not exist on disk).
inline void resolve(const char* guestPath, const std::string& nativePath, std::uint32_t id, std::uint64_t size, bool ok) {
    emit("[%sreq] %s id=0x%x size=0x%llx path=%s\n",
         ok ? "" : "FAIL ", guestPath ? guestPath : "?", id,
         static_cast<unsigned long long>(size), nativePath.c_str());
}

inline void read(std::uint32_t id, const char* path, std::uint64_t offset, std::uint64_t size, std::uint64_t destination, std::uint64_t ms) {
    emit("[%sreq] DONE id=0x%x off=0x%llx size=0x%llx dst=0x%llx %ums path=%s\n",
         "", id, static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size),
         static_cast<unsigned long long>(destination), static_cast<unsigned long long>(ms),
         path ? path : "?");
}

inline void openResult(const char* guestPath, int fd, int flags, bool ok) {
    emit("[%sreq] %s fd=%d flags=0x%X path=%s\n", ok ? "" : "FAIL ", guestPath ? guestPath : "?", fd, flags);
}

}  // namespace Aps5RequestLog
