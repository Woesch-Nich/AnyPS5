#ifndef CORE_LIBS_PRX_LIBKERNEL_PTHREAD_INCLUDE_JOBDIAG_HPP
#define CORE_LIBS_PRX_LIBKERNEL_PTHREAD_INCLUDE_JOBDIAG_HPP

// ~2 ms after GATE is published and dereferences a not-yet-published base global.
//
//   APS5_JOBDIAG=1   log thread create + every wait/signal on the guest sync objects,
//                    tick-stamped (same steady_clock ms base as the libc firstwatch trap).
//   APS5_JOBGATE=1   experiment (a): before a job thread runs its entry, spin (bounded) until
//                    the AGC base globals are all non-zero. Default OFF, local only, not upstream.
//
// Job threads are identified by their Windows thread description (set from the name the game
// passes to scePthreadCreate) matching APS5_JOBNAME (default "Job"). The base globals for the
// gate are read from the main module at the RVAs in APS5_JOBGATE_GLOBALS

//
// Everything is env-gated; with neither env var set the hooks are single getenv-cached branches
// and do nothing.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <exception>
#include <typeinfo>
#include <array>
#include <atomic>
#include <cstdarg>
#include <initializer_list>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace aps5::jobdiag {

// steady_clock ms, the same base the libc firstwatch trap and the AGC audit line use, so a
// [jobdiag] event and a [firstwatch] FIRST WRITE line are directly comparable.
inline std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline bool DiagEnabled() {
    static const bool on = std::getenv("APS5_JOBDIAG") != nullptr;
    return on;
}
inline bool GateEnabled() {
    static const bool on = std::getenv("APS5_JOBGATE") != nullptr;
    return on;
}

// ~0.5 ms and logs each change with sub-ms steady_clock. No page protection, so it CANNOT
// delay the writers. Gives the true GATE -> BASE_A -> BASE_C order + gaps.
inline bool PollEnabled() {
    static const bool on = std::getenv("APS5_JOBDIAG_POLL") != nullptr;
    return on;
}
inline void StartPoller();

// (default 2000) ms snapshots each Job* guest thread: SuspendThread + GetThreadContext +
// GetThreadTimes, module-resolves the RIP, logs it, resumes. Decides wait-vs-compute for the
// 140 s pre-GATE silence. APS5_JOBRIP=1 to enable; runs even with APS5_JOBDIAG off.
inline bool RipSamplerEnabled() {
    static const bool on = std::getenv("APS5_JOBRIP") != nullptr;
    return on;
}
inline void StartRipSampler();

// An uncaught C++ exception in our libs reaches std::terminate and kills the process with all
// buffered output lost (MSVCRT path: terminate -> abort() -> exit(3); mingw libstdc++ path:
// terminate -> RaiseException(0x20474343) -> exit(67) — see the ResourceMaterializer.cpp
// comments). The guest exit hooks cannot see either: the game never calls exit.
// We install, from our prx (one state instance per DLL via the C++17 inline variable below):
//   [host-term]      what() + type + 16 module-resolved stack frames, then delegate to the
//                    PREVIOUS terminate handler (preserves the original exit code 3/67);
//   [host-sigabrt]   logged once, then re-raised with the default handler;
//   [host-seh]       the final non-continuable exception's code/address (the SEH view of the
//                    terminate raise), then default handling.
// Armed at DLL load and re-armed after 2 s (beats any handler the game sets during init).
#ifdef _WIN32
inline void RipToSymbol(std::uintptr_t rip, char* out, std::size_t outSize);
struct HostTermState {
    std::terminate_handler prev{};
    std::terminate_handler ours{};
    std::atomic<bool> armed{false};
    std::atomic<bool> termLogged{false};
    std::atomic<bool> sigabrtLogged{false};
    void LogTerm() noexcept {
        if (termLogged.exchange(true)) return;
        char what[256] = "(no current exception)";
        char type[128] = "?";
        int have = 0;
        try {
            if (auto eptr = std::current_exception()) {
                std::rethrow_exception(eptr);
                have = 1;
            }
        } catch (const std::exception& e) {
            have = 1;
            std::snprintf(what, sizeof(what), "%.248s", e.what());
            std::snprintf(type, sizeof(type), "%s", typeid(e).name());
        } catch (...) {
            have = 1;
            std::snprintf(what, sizeof(what), "non-std::exception");
        }
        std::fprintf(stderr, "[host-term] terminate have=%d type=%s what=\"%s\"\n", have, type, what);
        void* frames[16];
        const DWORD n = CaptureStackBackTrace(1, 16, frames, nullptr);
        for (DWORD i = 0; i < n; ++i) {
            char sym[160];
            RipToSymbol(reinterpret_cast<std::uintptr_t>(frames[i]), sym, sizeof(sym));
            std::fprintf(stderr, "[host-term]   #%lu %s\n", i, sym);
        }
        std::fflush(stderr);
    }
};
inline HostTermState gHostTerm;
inline void HostTermHandler() noexcept {
    // terminate handler: name the exception once, then delegate to the PREVIOUS handler so
    // the original exit code (3 / 67) survives the delegation chain.
    gHostTerm.LogTerm();
    gHostTerm.prev();
}
inline void InstallHostExceptionHandlers() {
    if (!gHostTerm.armed.exchange(true)) {
        std::signal(SIGABRT, [](int) {
            if (!gHostTerm.sigabrtLogged.exchange(true)) {
                std::fprintf(stderr, "[host-sigabrt] SIGABRT received\n");
                std::fflush(stderr);
            }
            std::signal(SIGABRT, SIG_DFL);
            std::abort();
        });
        SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* p) -> LONG {
            std::fprintf(stderr, "[host-seh] unhandled code=0x%08lx addr=%p flags=0x%lx\n",
                         p->ExceptionRecord->ExceptionCode, p->ExceptionRecord->ExceptionAddress,
                         p->ExceptionRecord->ExceptionFlags);
            std::fflush(stderr);
            return EXCEPTION_EXECUTE_HANDLER;
        });

        // [host-term] line — either the engine installed its own (non-chaining) terminate
        // handler after our 2 s re-arm, or the exit is a plain exit(N) that never goes
        // through std::terminate. Disambiguate: atexit fires ONLY via exit() (not _exit/
        // TerminateProcess); quick_exit likewise; the low-priority VEH sees the raise even
        // if something else handles it; the re-arm thread keeps OUR terminate handler last
        // no matter when the engine installs its own.
        std::atexit([]() {
            std::fprintf(stderr, "[host-exit] atexit fired (exit() path)\n");
            std::fflush(stderr);
        });
        std::at_quick_exit([]() {
            std::fprintf(stderr, "[host-quickexit] at_quick_exit fired\n");
            std::fflush(stderr);
        });
        AddVectoredExceptionHandler(0, [](PEXCEPTION_POINTERS p) -> LONG {
            const DWORD c = p->ExceptionRecord->ExceptionCode;
            if (c == 0x20474343u || c == 0xE0667434u) {
                void* frames[16];
                const DWORD n = CaptureStackBackTrace(2, 16, frames, nullptr);
                std::fprintf(stderr, "[host-veh] code=0x%08lx addr=%p tid=%lu\n",
                             static_cast<unsigned long>(c), p->ExceptionRecord->ExceptionAddress,
                             static_cast<unsigned long>(GetCurrentThreadId()));
                for (DWORD i = 0; i < n; ++i) {
                    char sym[160];
                    RipToSymbol(reinterpret_cast<std::uintptr_t>(frames[i]), sym, sizeof(sym));
                    std::fprintf(stderr, "[host-veh]   #%lu %s\n", static_cast<unsigned long>(i), sym);
                }
                std::fflush(stderr);
            }
            return EXCEPTION_CONTINUE_SEARCH;
        });
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            for (;;) {
                Sleep(5000);
                gHostTerm.prev = std::get_terminate();
                std::set_terminate(&HostTermHandler);
            }
            return 0;
        }, nullptr, 0, nullptr);
    }
    // Re-armable: prev is re-captured each time so our handler stays last.
    gHostTerm.prev = std::get_terminate();
    gHostTerm.ours = &HostTermHandler;
    std::set_terminate(&HostTermHandler);
}
inline void MaybeInstallHostExceptionHandlers() {
    static std::atomic<bool> lateDone{false};
    if (lateDone.load(std::memory_order_relaxed)) return;
    static const auto t0 = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2)) return;
    if (!lateDone.exchange(true, std::memory_order_relaxed)) InstallHostExceptionHandlers();
}
struct HostTermPreload {
    HostTermPreload() { InstallHostExceptionHandlers(); }
};
inline const HostTermPreload gHostTermPreload;
#else
inline void InstallHostExceptionHandlers() {}
inline void MaybeInstallHostExceptionHandlers() {}
#endif

// steady_clock microseconds, same clock/epoch as NowMs (NowMs == NowUs/1000).
inline std::uint64_t NowUs() {
    MaybeInstallHostExceptionHandlers();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The guest thread name (Windows thread description), "" if none.
inline const char* ThreadName() {
#ifdef _WIN32
    static thread_local std::string name;
    if (name.empty()) {
        PWSTR wide = nullptr;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &wide)) && wide) {
            std::wstring w(wide);
            CoTaskMemFree(wide);
            name.assign(w.begin(), w.end());
        }
    }
    return name.c_str();
#else
    return "";
#endif
}

inline bool IsJobThread(const char* name) {
    static const char* prefix = [] {
        const auto* e = std::getenv("APS5_JOBNAME");
        return e ? e : "Job";
    }();
    if (name == nullptr || name[0] == '\0') return false;
    return std::strncmp(name, prefix, std::strlen(prefix)) == 0;
}

inline void Event(const char* kind, std::uintptr_t obj, const char* extra = nullptr) {
    if (!DiagEnabled()) return;
    static std::atomic<bool> pollStarted{false};
    if (PollEnabled() && !pollStarted.exchange(true, std::memory_order_relaxed)) StartPoller();
    static std::atomic<bool> ripStarted{false};
    if (RipSamplerEnabled() && !ripStarted.exchange(true, std::memory_order_relaxed)) StartRipSampler();
    static std::mutex m;
    static std::unordered_map<std::string, std::array<std::uint64_t, 3>> seen;  // key -> {count, lastMs, suppressed}
    std::lock_guard lock(m);
#ifdef _WIN32
    const auto tid = static_cast<unsigned long>(GetCurrentThreadId());
#else
    const auto tid = 0u;
#endif
    const char* name = ThreadName();
    const bool job = IsJobThread(name);
    std::string key;
    key.reserve(64);
    key += std::to_string(tid);
    key += kind;
    auto& s = seen[key];


    // APS5_JOBDIAG_SEMA_SAMPLE=1 turns sampling off. Everything else stays at full rate.
    {
        static const std::uint64_t semaSample = [] {
            const char* e = std::getenv("APS5_JOBDIAG_SEMA_SAMPLE");
            std::uint64_t n = 16;
            if (e && *e) n = std::strtoull(e, nullptr, 10);
            return n == 0 ? 1 : n;
        }();
        static std::unordered_map<std::string, std::uint64_t> skip;
        if (job && semaSample > 1 &&
            (std::strcmp(kind, "sema-signal") == 0 || std::strcmp(kind, "sema-wait") == 0)) {
            auto& sk = skip[key];
            if (++sk < semaSample) {
                s[2]++;
                return;
            }
            sk = 0;
        }
    }
    const auto now = NowMs();
    // job threads: log everything (they are the subject); other threads: first 64 events fully,
    // then at most one line per 500 ms carrying the suppressed count (avoids 120 fps poll floods)
    if (!job && s[0] >= 64) {
        if (now - s[1] < 500) {
            s[2]++;
            return;
        }
        if (s[2] > 0) s[2] = 0;
    }
    s[0]++;
    s[1] = now;
    char line[512];
    if (s[2] > 0) {
        std::snprintf(line, sizeof(line), "[jobdiag] %llu ms tid=%lu '%s' %s obj=%p%s%s (suppressed %llu between lines)\n",
                       static_cast<unsigned long long>(now), tid, name, kind, reinterpret_cast<void*>(obj),
                       extra ? " " : "", extra ? extra : "", static_cast<unsigned long long>(s[2]));
    } else {
        std::snprintf(line, sizeof(line), "[jobdiag] %llu ms tid=%lu '%s' %s obj=%p%s%s\n",
                       static_cast<unsigned long long>(now), tid, name, kind, reinterpret_cast<void*>(obj),
                       extra ? " " : "", extra ? extra : "");
    }
    std::fwrite(line, 1, std::strlen(line), stderr);
}


// libc (guest arena commit/release/alloc). Gated by APS5_TRACE_MEMORY; when
// APS5_MEM_REGION=0xlo,0xhi is set only calls overlapping that range are emitted.
// Same line format as Event (same ms base, tid, thread name) so the two compose,
// and NOT subject to Event's flood sampler — mapping calls are rare and each matters.
namespace {
struct MemRegionFilter {
    bool on = false;
    std::uintptr_t lo = 0;
    std::uintptr_t hi = 0;
    MemRegionFilter() {
        const char* e = std::getenv("APS5_MEM_REGION");
        if (e && *e) {
            char* end = nullptr;
            lo = std::strtoull(e, &end, 0);
            if (end && *end == ',') hi = std::strtoull(end + 1, nullptr, 0);
            if (hi > lo) on = true;
        }
    }
    bool Hit(std::uintptr_t addr, std::size_t len) const {
        return !on || addr < hi && addr + len > lo;
    }
};
inline bool MemTraceEnabled() {
    static const bool enabled = std::getenv("APS5_TRACE_MEMORY") != nullptr;
    return enabled;
}
}  // namespace

// a208 (#A207-answer): ordering probe + lazy-commit EXPERIMENT (not a fix).
//  - NoteLastWait: every guest sync-wait NID records (ts, kind, obj) on the calling thread.
//  - LazyCommitFilter (VEH): a WRITE fault into a reserved, uncommitted guest-arena page is
//    committed on demand (PS5-RAM semantics) and logged as mem-lazycommit with the faulting
//    thread's last wait + dt. If the run then survives, later slice maps log already= pages.
//  Gated on APS5_TRACE_MEMORY. Per-DLL registration (libkernel copy is the informed one).
inline void MemLine(std::uintptr_t addr, std::size_t len, const char* verb, const char* format, ...);
#ifdef _WIN32
inline void NoteSharedLastWait(const char* kind, std::uintptr_t obj, std::uintptr_t tid);  // fwd (defined below)
struct LastWaitRec {
    std::uint64_t ts = 0;
    const char* kind = "";
    std::uintptr_t obj = 0;
};
inline thread_local LastWaitRec TlsLastWait;
inline const LastWaitRec* LastWaitOf() {
    return &TlsLastWait;
}
inline void NoteLastWait(const char* kind, std::uintptr_t obj) {
    if (!MemTraceEnabled()) return;
    TlsLastWait.ts = NowMs();
    TlsLastWait.kind = kind;
    TlsLastWait.obj = obj;
    NoteSharedLastWait(kind, obj, 0);  // 0 = current thread (cross-image, for VEH/crash reads)
}
// thread_local is per-DLL: the wait NIDs live in libkernel, the VEH and the FATAL
// reporter live in libc — a208's afterwait=none everywhere was reading the wrong
// image's TLS. Shared slot table (tid-indexed, per-field atomics, tear-tolerant).
#ifdef _WIN32
struct SharedLastWaitSlot {
    std::atomic<std::uintptr_t> kind{0};
    std::atomic<std::uintptr_t> obj{0};
    std::atomic<std::uint64_t> ts{0};
};
inline SharedLastWaitSlot& SharedLastWaitSlotFor(std::uintptr_t tid) {
    static std::array<SharedLastWaitSlot, 512> slots;  // function-static: ONE table per image
    return slots[tid & 511u];
}
#endif
inline void NoteSharedLastWait(const char* kind, std::uintptr_t obj, std::uintptr_t tid) {
#ifdef _WIN32
    auto& slot = SharedLastWaitSlotFor(tid == 0 ? GetCurrentThreadId() : tid);
    slot.kind.store(reinterpret_cast<std::uintptr_t>(kind), std::memory_order_relaxed);
    slot.obj.store(obj, std::memory_order_relaxed);
    slot.ts.store(NowMs(), std::memory_order_release);  // publish marker last
#endif
}
inline bool SharedLastWaitOf(std::uintptr_t tid, const char** kind, std::uintptr_t* obj, std::uint64_t* ts) {
#ifdef _WIN32
    const auto& slot = SharedLastWaitSlotFor(tid == 0 ? GetCurrentThreadId() : tid);
    const auto stamp = slot.ts.load(std::memory_order_acquire);
    if (stamp == 0) return false;
    *ts = stamp;
    *obj = slot.obj.load(std::memory_order_relaxed);
    *kind = reinterpret_cast<const char*>(slot.kind.load(std::memory_order_relaxed));
    return true;
#else
    (void)tid; (void)kind; (void)obj; (void)ts; return false;
#endif
}
inline LONG WINAPI LazyCommitFilter(EXCEPTION_POINTERS* ep) {
    // EXCEPTION_EXECUTED_HANDLER (the winnt.h constant is missing from the SDK)
    constexpr LONG kExecutedHandler = 2;
    auto& ex = *ep->ExceptionRecord;
    if (ex.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || ex.NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    if (ex.ExceptionInformation[0] != 2) return EXCEPTION_CONTINUE_SEARCH;  // writes only
    const auto addr = ex.ExceptionInformation[1];
    if (addr < 0x80000000ull || (addr & 0xfffull)) return EXCEPTION_CONTINUE_SEARCH;  // >= 128 MB only
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) return EXCEPTION_CONTINUE_SEARCH;
    if (addr < 0x200000000ull) {

        // LOG-ONLY. Record what the page was and the faulting thread's last wait, then let the
        // game's own SEH handle it (zero behaviour change).
        if (mbi.State == MEM_RESERVE) {
            const char* lwKind = nullptr;
            std::uintptr_t lwObj = 0;
            std::uint64_t lwTs = 0;
            if (SharedLastWaitOf(0, &lwKind, &lwObj, &lwTs)) {
                MemLine(addr, 0x1000, "mem-lazywatch", "fam=host state=reserve type=%s prot=0x%x lastwait=%s obj=0x%llx dt=%llums",
                        mbi.Type == MEM_PRIVATE ? "private" : (mbi.Type == MEM_MAPPED ? "mapped" : "other"), static_cast<unsigned>(mbi.Protect), lwKind, static_cast<unsigned long long>(lwObj), static_cast<unsigned long long>(NowMs() - lwTs));
            } else {
                MemLine(addr, 0x1000, "mem-lazywatch", "fam=host state=reserve type=%s prot=0x%x lastwait=none", mbi.Type == MEM_PRIVATE ? "private" : (mbi.Type == MEM_MAPPED ? "mapped" : "other"), static_cast<unsigned>(mbi.Protect));
            }
        } else if (mbi.State == MEM_COMMIT && (mbi.Protect == PAGE_NOACCESS)) {
            const char* lwKind = nullptr;
            std::uintptr_t lwObj = 0;
            std::uint64_t lwTs = 0;
            if (SharedLastWaitOf(0, &lwKind, &lwObj, &lwTs)) {
                MemLine(addr, 0x1000, "mem-lazywatch", "fam=host state=commit prot=noaccess lastwait=%s obj=0x%llx dt=%llums", lwKind, static_cast<unsigned long long>(lwObj), static_cast<unsigned long long>(NowMs() - lwTs));
            } else {
                MemLine(addr, 0x1000, "mem-lazywatch", "fam=host state=commit prot=noaccess lastwait=none");
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Guest-arena family (>= 8.59 GB): commit on demand (a208 experiment). File-backed arena
    // maps are MEM_MAPPED (a208's gate only took MEM_PRIVATE and may have missed them); accept both.
    if (mbi.State != MEM_RESERVE || (mbi.Type != MEM_PRIVATE && mbi.Type != MEM_MAPPED)) return EXCEPTION_CONTINUE_SEARCH;
    if (VirtualAlloc(reinterpret_cast<void*>(addr), 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE) == nullptr) return EXCEPTION_CONTINUE_SEARCH;
    const char* lwKind = nullptr;
    std::uintptr_t lwObj = 0;
    std::uint64_t lwTs = 0;
    if (SharedLastWaitOf(0, &lwKind, &lwObj, &lwTs)) {
        MemLine(addr, 0x1000, "mem-lazycommit", "fam=guest lastwait=%s obj=0x%llx dt=%llums", lwKind, static_cast<unsigned long long>(lwObj), static_cast<unsigned long long>(NowMs() - lwTs));
    } else {
        MemLine(addr, 0x1000, "mem-lazycommit", "fam=guest lastwait=none");
    }
    return kExecutedHandler;
}
inline void EnsureLazyCommitArmed() {
    static const bool armed = [] {
        AddVectoredExceptionHandler(1, LazyCommitFilter);
        return true;
    }();
    (void)armed;
}
#endif

inline void MemLine(std::uintptr_t addr, std::size_t len, const char* verb, const char* format, ...) {
    if (!MemTraceEnabled()) return;
#ifdef _WIN32
    EnsureLazyCommitArmed();
#endif
    static const MemRegionFilter filter;
    if (!filter.Hit(addr, len)) return;
    unsigned long tid = 0;
#ifdef _WIN32
    tid = GetCurrentThreadId();
#endif
    char extra[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(extra, sizeof(extra), format, args);
    va_end(args);
    std::fprintf(stderr, "[jobdiag] %llu ms tid=%lu '%s' %s obj=0x%llx len=0x%zx %s\n",
                 static_cast<unsigned long long>(NowMs()), tid, ThreadName(), verb,
                 static_cast<unsigned long long>(addr), len, extra);
}

inline void MemLineRaw(const char* verb, const char* format, ...) {
    if (!MemTraceEnabled()) return;
#ifdef _WIN32
    EnsureLazyCommitArmed();
#endif
    static const MemRegionFilter filter;
    if (filter.on) return;  // non guest-VA events only in unfiltered mode
    unsigned long tid = 0;
#ifdef _WIN32
    tid = GetCurrentThreadId();
#endif
    char extra[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(extra, sizeof(extra), format, args);
    va_end(args);
    std::fprintf(stderr, "[jobdiag] %llu ms tid=%lu '%s' %s %s\n",
                 static_cast<unsigned long long>(NowMs()), tid, ThreadName(), verb, extra);
}

// a211 (#A209-answer/#A211): memory/budget NID census — first 40 s, args AND our return
// values. Diff a provisioned run vs a non-provisioned one: the first call whose RESULT
// differs is the cause of the per-run provisioning split.
struct NidCensusSlot {
    const char* fn = nullptr;
    std::atomic<std::uint32_t> total{0};
    std::atomic<std::uint32_t> logged{0};
};
inline NidCensusSlot& NidCensusSlotFor(const char* fn) {
    static NidCensusSlot table[24];
    for (auto& s : table)
        if (s.fn == fn) return s;
    for (auto& s : table)
        if (s.fn == nullptr) {
            s.fn = fn;  // benign first-use race: all racers store the same value
            return s;
        }
    return table[0];
}
constexpr std::uint64_t kNidCensusWindowMs = 40000;
constexpr std::uint32_t kNidCensusCap = 1000;
inline std::uint64_t NidCensusT0() {
    static const std::uint64_t t0 = NowMs();
    return t0;
}
struct NidCensusFrame {
    const char* fn = nullptr;
    std::uintptr_t args[6] = {};
};
struct NidCensusTls {
    NidCensusFrame stack[3] = {};
    int depth = 0;
};
inline NidCensusTls& NidCensusTlsRef() {
    static thread_local NidCensusTls tls;
    return tls;
}
inline bool NidCensusBegin(const char* fn, std::initializer_list<std::uintptr_t> args) {
    if (!MemTraceEnabled()) return false;
    if (NowMs() - NidCensusT0() > kNidCensusWindowMs) return false;
    auto& slot = NidCensusSlotFor(fn);
    slot.total.fetch_add(1, std::memory_order_relaxed);
    if (slot.logged.fetch_add(1, std::memory_order_relaxed) >= kNidCensusCap) return false;
    NidCensusFrame frame{fn, {}};
    { std::size_t i = 0;
      for (auto v : args)
          if (i < 6) frame.args[i++] = v; }
    auto& tls = NidCensusTlsRef();
    if (tls.depth < 3) tls.stack[tls.depth++] = frame;
    char line[192];
    std::snprintf(line, sizeof(line), "fn=%s a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx a4=0x%llx a5=0x%llx",
                  fn, (unsigned long long)frame.args[0], (unsigned long long)frame.args[1], (unsigned long long)frame.args[2],
                  (unsigned long long)frame.args[3], (unsigned long long)frame.args[4], (unsigned long long)frame.args[5]);
    MemLineRaw("mem-nidcall", "%s", line);
    return true;
}
inline void NidCensusEnd(const char* fn, std::uintptr_t ret, std::initializer_list<std::uintptr_t> out = {}) {
    if (!MemTraceEnabled()) return;
    auto& tls = NidCensusTlsRef();
    if (tls.depth == 0) return;  // Begin was not enabled (window/cap) — nothing paired
    const NidCensusFrame frame = tls.stack[--tls.depth];
    if (frame.fn != fn) {  // desynced nesting: resync, drop this return
        tls.depth = 0;
        return;
    }
    std::uintptr_t o[3] = {};
    { std::size_t i = 0;
      for (auto v : out)
          if (i < 3) o[i++] = v; }
    char line[192];
    std::snprintf(line, sizeof(line), "fn=%s ret=0x%llx o1=0x%llx o2=0x%llx o3=0x%llx", fn, (unsigned long long)ret,
                  (unsigned long long)o[0], (unsigned long long)o[1], (unsigned long long)o[2]);
    MemLineRaw("mem-nidret", "%s", line);
}
// a213 (#A213-answer): exception-path return — the census End is skipped when the impl throws,
// which erases the call from the log. Log a synthetic ret so a throwing NID is visible.
inline void NidCensusEndNote(const char* fn, const char* note) {
    if (!MemTraceEnabled()) return;
    auto& tls = NidCensusTlsRef();
    if (tls.depth == 0) return;
    const NidCensusFrame frame = tls.stack[--tls.depth];
    if (frame.fn != fn) {
        tls.depth = 0;
        return;
    }
    char line[256];
    std::snprintf(line, sizeof(line), "fn=%s ret=0x8000000000000000 note=%s", fn, note ? note : "ex");
    MemLineRaw("mem-nidret", "%s", line);
}
// SetVirtualRangeName carries the mapping's name — the attribution key for Map* lines.
inline void NidCensusName(const char* fn, const char* name) {
    if (!MemTraceEnabled()) return;
    char line[160];
    std::snprintf(line, sizeof(line), "fn=%s name=%s", fn, name ? name : "(null)");
    MemLineRaw("mem-nidname", "%s", line);
}


struct BaseGlobals {
    std::atomic<std::uint64_t>* a = nullptr;
    std::atomic<std::uint64_t>* c = nullptr;
    std::atomic<std::uint64_t>* g = nullptr;
    bool ok = false;
};
inline const BaseGlobals& Globals() {
    static const BaseGlobals g = [] {
        BaseGlobals r;
        if (!GateEnabled() && !PollEnabled()) return r;
#ifdef _WIN32
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto* spec = std::getenv("APS5_JOBGATE_GLOBALS");
        const char* def = "";
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", spec ? spec : def);
        std::uint64_t rv[3] = {0, 0, 0};
        if (std::sscanf(buf, "%lx,%lx,%lx", &rv[0], &rv[1], &rv[2]) != 3) return r;
        r.a = reinterpret_cast<std::atomic<std::uint64_t>*>(base + rv[0]);
        r.c = reinterpret_cast<std::atomic<std::uint64_t>*>(base + rv[1]);
        r.g = reinterpret_cast<std::atomic<std::uint64_t>*>(base + rv[2]);
        r.ok = true;
        std::fprintf(stderr, "[jobbase] armed: BASE_A@0x%llx BASE_C@0x%llx GATE@0x%llx (main module base 0x%llx)\n",
                     static_cast<unsigned long long>(base + rv[0]), static_cast<unsigned long long>(base + rv[1]),
                     static_cast<unsigned long long>(base + rv[2]), static_cast<unsigned long long>(base));
#endif
        return r;
    }();
    return g;
}


// Reads BASE_A/BASE_C/GATE every ~0.5 ms (spin-yield hybrid, ~30% of one core), logs each
// CHANGE with sub-ms steady_clock — same clock/epoch as NowMs, so ms lines and us lines are
// directly comparable. No page protection: it cannot delay the writers. Stops 5 s after all
// three are non-zero (bounds CPU + log once the interesting burst is captured).
inline void StartPoller() {
    const auto& g = Globals();
    if (!g.ok) return;
    std::thread([g]() {
        std::fprintf(stderr,
                     "[jobpoll] started: BASE_A@0x%llx BASE_C@0x%llx GATE@0x%llx, polling ~every 0.5 ms "
                     "(no page protection, cannot delay writers)\n",
                     static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g.a)),
                     static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g.c)),
                     static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g.g)));
        auto pa = g.a->load(std::memory_order_relaxed);
        auto pc = g.c->load(std::memory_order_relaxed);
        auto pg = g.g->load(std::memory_order_relaxed);
        std::fprintf(stderr, "[jobpoll] %.3f ms initial a=0x%llx c=0x%llx g=0x%llx\n", NowUs() / 1000.0,
                     static_cast<unsigned long long>(pa), static_cast<unsigned long long>(pc),
                     static_cast<unsigned long long>(pg));
        auto lastAllUs = 0ull;
        while (true) {
            // ~0.5 ms hybrid: spin-yield 250 us, then a short sleep (Windows sleep_for(100us)
            // resolves to ~1 ms without timeBeginPeriod, so keep the spin dominant).
            auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - t0 < std::chrono::microseconds(250))
                std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::microseconds(250));
            const auto na = g.a->load(std::memory_order_relaxed);
            const auto nc = g.c->load(std::memory_order_relaxed);
            const auto ng = g.g->load(std::memory_order_relaxed);
            if (na != pa) {
                std::fprintf(stderr, "[jobpoll] %.3f ms BASE_A 0x%llx -> 0x%llx\n", NowUs() / 1000.0,
                             static_cast<unsigned long long>(pa), static_cast<unsigned long long>(na));
                pa = na;
            }
            if (nc != pc) {
                std::fprintf(stderr, "[jobpoll] %.3f ms BASE_C 0x%llx -> 0x%llx\n", NowUs() / 1000.0,
                             static_cast<unsigned long long>(pc), static_cast<unsigned long long>(nc));
                pc = nc;
            }
            if (ng != pg) {
                std::fprintf(stderr, "[jobpoll] %.3f ms GATE   0x%llx -> 0x%llx\n", NowUs() / 1000.0,
                             static_cast<unsigned long long>(pg), static_cast<unsigned long long>(ng));
                pg = ng;
            }
            if (na && nc && ng) {
                if (lastAllUs == 0) lastAllUs = NowUs();
                if (NowUs() - lastAllUs > 5000000) {
                    std::fprintf(stderr, "[jobpoll] all three published for 5 s, stopping poller\n");
                    break;
                }
            } else {
                lastAllUs = 0;
            }
        }
    }).detach();
}


// read of BASE_A by a job thread. No behaviour change: each probe is a 0xCC that is
// restored, single-stepped (TF), and re-armed from the VEH. Gated on APS5_INITREAD=1.
//
//   Sites (main-module RVAs, env-overridable):




//
//   Lines: [jobdiag] <ms> ms tid=<tid> '<name>' probe-<label> [value=0x..] [note=...]
#ifdef _WIN32
struct InitReadProbe {
    struct Site {
        std::uint64_t rva = 0;
        const char* label = nullptr;
        std::atomic<std::uint8_t*> addr{nullptr};
        std::atomic<std::uint8_t> orig{0};
        std::atomic<bool> stepped{false};
        std::atomic<bool> armed{false};
        int valueSlot = -1;  // 0=BASE_A 1=BASE_C, -1 = no value log
        std::atomic<std::uint64_t> hits{0};
    };
    static constexpr int N = 4;
    Site sites[N];
    std::atomic<bool> ready{false};
    std::atomic<bool> handlerOn{false};
    std::atomic<bool> firstReadLogged{false};
    std::uint64_t readTids[32]{};  // first-hit-per-tid for probe-read-a (benign race: dup line)
    std::mutex armLock;
    std::uint64_t valueRva[2] = {};  // BASE_A, BASE_C (APS5_JOBGATE_GLOBALS style)
    std::atomic<std::uintptr_t> mainBase{0};
};
inline InitReadProbe& InitProbe() {
    static InitReadProbe p{};
    return p;
}
inline bool InitProbeEnabled() {
    static const bool on = std::getenv("APS5_INITREAD") != nullptr;
    return on;
}
inline std::uint64_t ParseProbeRva(const char* env, std::uint64_t def) {
    const char* e = std::getenv(env);
    if (e && *e) return std::strtoull(e, nullptr, 16);
    return def;
}
LONG CALLBACK InitProbeFilter(EXCEPTION_POINTERS* ep);
inline void LogProbeValue(const char* label, std::uint64_t tid, const char* tname, int slot) {
    auto& p = InitProbe();
    const auto base = p.mainBase.load(std::memory_order_relaxed);
    if (!base || p.valueRva[slot] == 0) return;
    const auto v = *reinterpret_cast<std::uint64_t*>(base + p.valueRva[slot]);
    std::fprintf(stderr, "[jobdiag] %llu ms tid=%lu '%s' probe-%s-value value=0x%llx\n",
                 static_cast<unsigned long long>(NowMs()), tid, tname, label, static_cast<unsigned long long>(v));
}
inline void EnsureInitProbeArmed() {
    if (!InitProbeEnabled()) return;
    auto& p = InitProbe();
    if (p.ready.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(p.armLock);
    if (p.ready.load(std::memory_order_relaxed)) return;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    p.mainBase.store(base, std::memory_order_relaxed);
    const char* gv = std::getenv("APS5_JOBGATE_GLOBALS");
    if (gv && *gv) {
        std::uint64_t r0 = 0, r1 = 0, r2 = 0;
        if (std::sscanf(gv, "%lx,%lx,%lx", &r0, &r1, &r2) == 3) {
            p.valueRva[0] = r0;
            p.valueRva[1] = r1;
        }
    }
    {
        const char* labels[4] = {"init-entry", "store-a", "store-c", "read-a"};
        const std::uint64_t rvas[4] = {ParseProbeRva("APS5_INITREAD_INIT", 0), ParseProbeRva("APS5_INITREAD_STORE", 0),
                                       ParseProbeRva("APS5_INITREAD_STOREC", 0), ParseProbeRva("APS5_INITREAD_READ", 0)};
        const int slots[4] = {-1, 0, 1, -1};
        for (int i = 0; i < InitReadProbe::N; ++i) {
            auto& s2 = p.sites[i];
            s2.rva = rvas[i];
            s2.label = labels[i];
            s2.valueSlot = slots[i];
        }
    }
    for (auto& s : p.sites) {
        if (!s.rva) continue;
        auto* pa = reinterpret_cast<std::uint8_t*>(base + s.rva);
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(pa, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS) {
            std::fprintf(stderr, "[jobdiag] initprobe: site %s @0x%llx not mappable, skipped\n", s.label, s.rva);
            continue;
        }
        DWORD oldProt = 0;
        if (!VirtualProtect(pa, 1, PAGE_EXECUTE_READWRITE, &oldProt)) continue;
        s.orig.store(*pa, std::memory_order_relaxed);
        *pa = 0xCC;
        VirtualProtect(pa, 1, mbi.Protect, &oldProt);
        s.addr.store(pa, std::memory_order_relaxed);
        s.armed.store(true, std::memory_order_relaxed);
        std::fprintf(stderr, "[jobdiag] initprobe: armed %s @0x%llx (orig byte 0x%02x)\n", s.label,
                     static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pa)), s.orig.load());
    }
    if (!p.handlerOn.exchange(true)) AddVectoredExceptionHandler(1, InitProbeFilter);
    p.ready.store(true, std::memory_order_release);
}
// Must be declared before EnsureInitProbeArmed uses it; defined right below.
inline LONG CALLBACK InitProbeFilter(EXCEPTION_POINTERS* ep) {
    auto& p = InitProbe();
    if (!p.ready.load(std::memory_order_acquire)) return EXCEPTION_CONTINUE_SEARCH;
    const auto code = ep->ExceptionRecord->ExceptionCode;
    const auto rip = reinterpret_cast<std::uintptr_t>(ep->ContextRecord->Rip);
    if (code == EXCEPTION_BREAKPOINT) {
        for (auto& s : p.sites) {
            if (!s.armed.load(std::memory_order_relaxed)) continue;
            if (rip != reinterpret_cast<std::uintptr_t>(s.addr.load(std::memory_order_relaxed))) continue;
            // Restore the original byte and single-step the real instruction; re-arm on #DB.
            auto* pa = s.addr.load(std::memory_order_relaxed);
            MEMORY_BASIC_INFORMATION mbi{};
            DWORD oldProt = 0;
            if (VirtualQuery(pa, &mbi, sizeof(mbi)) == sizeof(mbi) &&
                VirtualProtect(pa, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
                *pa = s.orig.load(std::memory_order_relaxed);
                VirtualProtect(pa, 1, mbi.Protect, &oldProt);
            }
            s.stepped.store(true, std::memory_order_relaxed);
            ep->ContextRecord->Rip += 1;
            ep->ContextRecord->EFlags |= 0x100;
            const auto hits = s.hits.fetch_add(1, std::memory_order_relaxed);
            auto tid = GetCurrentThreadId();
            auto tname = ThreadName();
            if (std::strcmp(s.label, "read-a") == 0) {
                bool firstTid = true;
                for (auto t : p.readTids) {
                    if (t == 0) {
                        t = tid;
                        break;
                    }
                    if (t == tid) firstTid = false;
                }
                if (firstTid)
                    std::fprintf(stderr, "[jobdiag] %llu ms tid=%lu '%s' probe-read-a note=first-hit-for-tid\n",
                                 static_cast<unsigned long long>(NowMs()), tid, tname);
            } else {
                std::fprintf(stderr, "[jobdiag] %llu ms tid=%lu '%s' probe-%s\n", static_cast<unsigned long long>(NowMs()),
                             tid, tname, s.label);
            }
            return 0 /* EXCEPTION_CONTINUE */;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == 0x80000004 /* STATUS_SINGLE_STEP */) {
        for (auto& s : p.sites) {
            if (!s.stepped.load(std::memory_order_relaxed)) continue;
            auto* pa = s.addr.load(std::memory_order_relaxed);
            s.stepped.store(false, std::memory_order_relaxed);
            // Re-arm regardless of where the stepped instruction jumped (a jmp target is fine);
            // the #DB fires on this thread immediately after the stepped instruction.
            if (*pa != s.orig.load(std::memory_order_relaxed)) continue;  // clobbered — probe lost
            MEMORY_BASIC_INFORMATION mbi{};
            DWORD oldProt = 0;
            if (VirtualQuery(pa, &mbi, sizeof(mbi)) == sizeof(mbi) &&
                VirtualProtect(pa, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
                *pa = 0xCC;
                VirtualProtect(pa, 1, mbi.Protect, &oldProt);
            }
            if (s.valueSlot >= 0) LogProbeValue(s.label, GetCurrentThreadId(), ThreadName(), s.valueSlot);
            return 0 /* EXCEPTION_CONTINUE */;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
inline void EnsureInitProbeArmed() {}
#endif

// Call from a job thread just before it runs its entry: if the gate is on and this thread is a job
// thread, spin (bounded, yielding) until all three base globals are non-zero. Logs the wait.
inline void GateJobStart(const char* name) {
    EnsureInitProbeArmed();
    if (!GateEnabled() || !IsJobThread(name)) return;
    const auto& g = Globals();
    if (!g.ok) return;
    static const std::uint64_t timeout = [] {
        const char* e = std::getenv("APS5_JOBGATE_TIMEOUT_MS");
        std::uint64_t t = 5000;
        if (e && *e) t = std::strtoull(e, nullptr, 10);
        if (t == 0) t = 5000;
        return t;
    }();
    auto start = NowMs();
    while (!(g.a->load(std::memory_order_acquire) && g.c->load(std::memory_order_acquire) &&
             g.g->load(std::memory_order_acquire))) {
        if (NowMs() - start > timeout) {
            std::fprintf(stderr, "[jobgate] %s: BOUNDED wait hit %llu ms; continuing (a=0x%llx c=0x%llx g=0x%llx)\n",
                         name, static_cast<unsigned long long>(timeout),
                         static_cast<unsigned long long>(g.a->load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g.c->load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g.g->load(std::memory_order_relaxed)));
            break;
        }
#ifdef _WIN32
        Sleep(0);
#else
        std::this_thread::yield();
#endif
    }
    if (g.a->load(std::memory_order_acquire))
        std::fprintf(stderr, "[jobgate] %s: released after %llu ms (a=0x%llx c=0x%llx g=0x%llx)\n", name,
                     static_cast<unsigned long long>(NowMs() - start),
                     g.a->load(std::memory_order_relaxed), g.c->load(std::memory_order_relaxed),
                     g.g->load(std::memory_order_relaxed));
}


// Every APS5_JOBRIP_MS ms (default 2000): CreateToolhelp32Snapshot over threads,
// SuspendThread + GetThreadContext + GetThreadTimes each Job* guest thread, log
//   [jobrip] <ms> ms tid=<tid> '<name>' rip=<module+off> kern=<ms> user=<ms>
// ResumeThread always (even on failure). Read-only on game state: each thread is
// suspended only for the GetThreadContext window (microseconds).
//

// LoadExec("exit"), catchReturnFromMain, abort) logs
//   [exithook] <ms> ms <path> code=<code> from=<return address, module-resolved>
// before doing anything, gated on APS5_TRACE_EXIT (already on) or APS5_EXITHOOK=1.

#ifdef _WIN32

inline void RipToSymbol(std::uintptr_t rip, char* out, std::size_t outSize);
inline void StackWalkTop(std::uintptr_t rip, std::uintptr_t rbp, std::size_t maxFrames,
                         std::string* out) {
    const auto hProc = GetCurrentProcess();
    auto readMem = [hProc](std::uintptr_t addr, void* dst, std::size_t n) -> bool {
        return ReadProcessMemory(hProc, reinterpret_cast<const void*>(addr), dst, n, nullptr) != 0;
    };
    std::string text;
    char frame[160];
    std::size_t count = 0;
    // frame 0 = the RIP itself
    char ripSym[128];
    RipToSymbol(rip, ripSym, sizeof(ripSym));
    std::snprintf(frame, sizeof(frame), "#0 %s", ripSym);
    text += frame;
    std::uintptr_t sp = rbp;
    std::uintptr_t fp = rbp;
    for (std::size_t f = 0; f < maxFrames; f++) {
        if (sp < 0x10000 || fp < 0x10000) break;
        std::uintptr_t nextFp = 0, ret = 0;
        if (!readMem(sp, &nextFp, sizeof(nextFp)) || !readMem(sp + 8, &ret, sizeof(ret))) break;
        if (nextFp < 0x10000 || nextFp >= 0x7fffffffffff) break;  // unmapped / non-canonical
        char sym[128];
        RipToSymbol(ret, sym, sizeof(sym));
        std::snprintf(frame, sizeof(frame), " #%zu %s", f + 1, sym);
        text += frame;
        count++;
        if (nextFp <= fp) break;  // not monotonically growing -> end of chain
        fp = nextFp;
        sp = nextFp + 8;
    }
    out->swap(text);
}


// no psapi/WINVER dependency: MODULEENTRY32A's modBaseAddr/modBaseSize are always present).
inline void RipToSymbol(std::uintptr_t rip, char* out, std::size_t outSize) {
    const auto hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        std::snprintf(out, outSize, "??+0x%llx (snap err)", static_cast<unsigned long long>(rip));
        return;
    }
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool found = false;
    if (Module32FirstW(hSnap, &me)) {
        do {
            const auto base = reinterpret_cast<std::uintptr_t>(me.modBaseAddr);
            if (rip >= base && rip < base + me.modBaseSize) {
                // w -> utf8, keep only the filename part (szModule is a fixed WCHAR[256] array)
                std::wstring w(me.szModule);
                const auto slash = w.find_last_of(L'\\');
                if (slash != std::wstring::npos) w.erase(0, slash + 1);
                std::string name(w.begin(), w.end());
                std::snprintf(out, outSize, "%s+0x%llx", name.c_str(),
                              static_cast<unsigned long long>(rip - base));
                found = true;
                break;
            }
        } while (Module32NextW(hSnap, &me));
    }
    CloseHandle(hSnap);
    if (!found) std::snprintf(out, outSize, "??+0x%llx (no module)", static_cast<unsigned long long>(rip));
}


// it is a Job* thread (the sampler's subjects), else nullptr.
inline const char* JobThreadNameOf(DWORD tid) {
    const auto h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) return nullptr;
    const char* result = nullptr;
    PWSTR wide = nullptr;
    if (SUCCEEDED(GetThreadDescription(h, &wide)) && wide) {
        std::wstring w(wide);
        CoTaskMemFree(wide);
        std::string s(w.begin(), w.end());
        if (IsJobThread(s.c_str())) {
            static thread_local std::string slot;
            slot = std::move(s);
            result = slot.c_str();
        }
    }
    CloseHandle(h);
    return result;
}
#endif

inline void StartRipSampler() {
    if (!RipSamplerEnabled()) return;
    static std::atomic<bool> started{false};
    if (started.exchange(true, std::memory_order_relaxed)) return;
#ifdef _WIN32
    std::uint64_t periodMs = 2000;
    if (const char* e = std::getenv("APS5_JOBRIP_MS"); e && *e) {
        periodMs = std::strtoull(e, nullptr, 10);
        if (periodMs == 0) periodMs = 2000;
    }
    std::thread([periodMs]() {
        std::fprintf(stderr, "[jobrip] started: sampling Job* threads every %llu ms (SuspendThread+GetThreadContext)\n",
                     static_cast<unsigned long long>(periodMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(periodMs)));
        for (std::uint64_t iter = 0; ; ++iter) {
            const auto hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (hSnap == INVALID_HANDLE_VALUE) {
                std::fprintf(stderr, "[jobrip] %.0f ms: snapshot failed (err=%lu), retrying\n", NowUs() / 1000.0,
                             static_cast<unsigned long>(GetLastError()));
                std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(periodMs)));
                continue;
            }
            // This mingw toolchain names the thread-snapshot entry THREADENTRY32 (Win10+ SDK
            // layout: dwSize/cntUsage/th32ThreadID/th32OwnerProcessID/tpBasePri/tpDeltaPri/dwFlags).
            THREADENTRY32 ts{};
            ts.dwSize = sizeof(ts);
            int sampled = 0;
            if (Thread32First(hSnap, &ts)) {
                do {
                    const char* name = JobThreadNameOf(ts.th32ThreadID);
                    if (name == nullptr) continue;
                    const auto h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, ts.th32ThreadID);
                    if (!h) continue;
                    const auto prev = SuspendThread(h);
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_FULL;
                    std::uintptr_t rip = 0;
                    std::uint64_t kernMs = 0, userMs = 0;
                    bool ok = false;
                    if (prev != static_cast<DWORD>(-1) && GetThreadContext(h, &ctx)) {
                        rip = static_cast<std::uintptr_t>(ctx.Rip);
                        FILETIME ftCreate, ftExit, ftKernel, ftUser;
                        if (GetThreadTimes(h, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
                            const auto toMs = [](const FILETIME& ft) {
                                ULARGE_INTEGER v;
                                v.HighPart = ft.dwHighDateTime;
                                v.LowPart = ft.dwLowDateTime;
                                return static_cast<std::uint64_t>(v.QuadPart / 10000);
                            };
                            kernMs = toMs(ftKernel);
                            userMs = toMs(ftUser);
                        }
                        ok = true;
                    }
                    if (prev != static_cast<DWORD>(-1)) ResumeThread(h);
                    CloseHandle(h);
                    if (!ok) continue;
                    char sym[128];
                    RipToSymbol(rip, sym, sizeof(sym));
                    std::fprintf(stderr, "[jobrip] %.0f ms tid=%lu '%s' rip=%s kern=%llu user=%llu\n",
                                 NowUs() / 1000.0, ts.th32ThreadID, name, sym,
                                 static_cast<unsigned long long>(kernMs), static_cast<unsigned long long>(userMs));

                    // walk the x64 RBP chain 4 frames up to name the guest wait call site.
                    if (ok && std::strncmp(sym, "ntdll", 5) == 0) {
                        std::string stackText;
                        StackWalkTop(static_cast<std::uintptr_t>(ctx.Rip),
                                     static_cast<std::uintptr_t>(ctx.Rbp), 4, &stackText);
                        std::fprintf(stderr, "[jobripstk] tid=%lu '%s' %s\n",
                                     ts.th32ThreadID, name, stackText.c_str());
                    }
                    sampled++;
                } while (Thread32Next(hSnap, &ts));
            }
            CloseHandle(hSnap);
            if (sampled == 0 && iter % 15 == 0) {
                std::fprintf(stderr, "[jobrip] %.0f ms: no Job* threads found yet\n", NowUs() / 1000.0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(periodMs)));
        }
    }).detach();
#endif
}


// shutdown work), so the line lands even if the teardown itself hangs.
inline void ExitHook(const char* path, int code, void* from) {
#ifdef _WIN32
    static const bool on = std::getenv("APS5_EXITHOOK") != nullptr || std::getenv("APS5_TRACE_EXIT") != nullptr;
    if (!on) return;
    char sym[128] = "?";
    if (from) RipToSymbol(reinterpret_cast<std::uintptr_t>(from), sym, sizeof(sym));
    std::fprintf(stderr, "[exithook] %llu ms %s code=%d from=%s\n", NowMs(), path, code, sym);
    std::fflush(stderr);
#else
    (void)path;
    (void)code;
    (void)from;
#endif
}

}  // namespace aps5::jobdiag

#endif
