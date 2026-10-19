
// Env: APS5_A55=1 enables. One 60 s summary: per-thread top-30 call counts + repeated
// error returns. Zero behavior change when unset (one getenv at first call).
#pragma once
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

namespace Aps5Call {

struct Rec { long count = 0; long errs = 0; long lastRet = 0; };
struct TRec { std::thread::id tid; std::unordered_map<int, Rec> recs; };

inline std::vector<TRec>& Tab() { static std::vector<TRec> t; return t; }
inline std::vector<std::string>& Names() { static std::vector<std::string> n; return n; }
inline std::mutex& Mtx() { static std::mutex m; return m; }
inline std::atomic<int>& CurThread() { static std::atomic<int> c{-1}; return c; }
inline std::atomic<bool>& Watching() { static std::atomic<bool> w{false}; return w; }

inline int Reg(const char* name) {
    if (std::getenv("APS5_A55") == nullptr) return 0;
    auto& n = Names();
    std::lock_guard<std::mutex> lk(Mtx());
    for (size_t i = 1; i < n.size(); i++) if (n[i] == name) return (int)i;
    n.push_back(name);
    return (int)n.size() - 1;
}

inline TRec& ThisRec() {
    auto& tab = Tab();
    int c = CurThread().load();
    if (c >= 0 && c < (int)tab.size()) return tab[c];
    std::lock_guard<std::mutex> lk(Mtx());
    for (size_t i = 0; i < tab.size(); i++)
        if (tab[i].tid == std::this_thread::get_id()) return tab[i];
    tab.push_back(TRec{});
    TRec& t = tab.back();
    t.tid = std::this_thread::get_id();
    CurThread().store((int)tab.size() - 1);
    return t;
}

inline void Begin(int id) {
    if (id == 0) return;
    ThisRec().recs[id].count++;
}

inline long Ret(int id, long code) {
    if (id == 0) return code;
    Rec& r = ThisRec().recs[id];
    r.lastRet = code;
    if (code < 0) r.errs++;
    return code;
}

inline void StartWatcher() {
    static std::atomic<bool> started{false};
    bool exp = false;
    if (!started.compare_exchange_strong(exp, true) || std::getenv("APS5_A55") == nullptr) return;
    std::thread([] {
        for (int s = 0; s < 60; s++) std::this_thread::sleep_for(std::chrono::seconds(1));
        auto& tab = Tab(); auto& names = Names();
        std::lock_guard<std::mutex> lk(Mtx());
        std::vector<std::pair<long, size_t>> order;
        for (size_t i = 0; i < tab.size(); i++) {
            long tot = 0;
            for (auto& kv : tab[i].recs) tot += kv.second.count;
            order.push_back({ tot, i });
        }
        std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::fprintf(stderr, "[a55] ===== A55 60s call census =====\n");
        for (auto& pr : order) {
            if (pr.first < 100) break;
            size_t i = pr.second;
            std::vector<std::pair<int, Rec*>> list;
            for (auto& kv : tab[i].recs) list.push_back({ kv.first, &kv.second });
            std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.second->count > b.second->count; });
            std::fprintf(stderr, "[a55] --- busy thread total=%ld (top 30) ---\n", (long)pr.first);
            for (size_t k = 0; k < list.size() && k < 30; k++) {
                std::fprintf(stderr, "[a55]  %s count=%ld errs=%ld lastRet=0x%lx\n",
                    names[list[k].first].c_str(), list[k].second->count, list[k].second->errs,
                    (unsigned long)list[k].second->lastRet);
            }
        }
        std::fprintf(stderr, "[a55] --- repeated error returns (errs>=2) ---\n");
        for (auto& t : tab) for (auto& kv : t.recs)
            if (kv.second.errs >= 2)
                std::fprintf(stderr, "[a55]  %s errs=%ld lastRet=0x%lx\n",
                    names[kv.first].c_str(), kv.second.errs, (unsigned long)kv.second.lastRet);
        std::fprintf(stderr, "[a55] ===== A55 end =====\n");
        std::fflush(stderr);
    }).detach();
}

} // namespace Aps5Call

// At function top:  APS5_A55_BEGIN("sceFoo")
#define APS5_A55_BEGIN(fname) static const int a55id = (Aps5Call::StartWatcher(), Aps5Call::Reg(fname)); Aps5Call::Begin(a55id)
// Replace `return X;` with:  return APS5_A55_END(X);  (X must be a comma-free expression)
#define APS5_A55_END(expr) Aps5Call::Ret(a55id, (long)(expr))
