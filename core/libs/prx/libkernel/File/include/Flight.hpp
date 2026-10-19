// A51 probe #3: per-thread in-flight host-call tracking.
// Instrumented host functions call Aps5Flight::enter()/leave() so that at any
// moment we can print which import call each live thread is still INSIDE.
// Gated by APS5_TRACE_FLIGHT=1; prints a summary every ~15 s and on summary().
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Aps5Flight {

class Entry {
  public:
    Entry(const char* name, int depth)
        : name(name ? name : "?"), depth(depth), entered(std::chrono::steady_clock::now()) {}
    std::string name;
    int depth;
    std::chrono::steady_clock::time_point entered;
};

class Tracker {
  public:
    static Tracker& instance() {
        static Tracker instance;
        return instance;
    }

    void enter(const char* name) {
        if (!enabled_)
            return;
        auto key = std::this_thread::get_id();
        std::lock_guard<std::mutex> lock(mutex_);
        auto& stack = stacks_[key];
        if (stack.size() > 16) {
            // Keep the stack bounded: drop the oldest frame.
            stack.erase(stack.begin());
        }
        stack.push_back(Entry(name, stack.size()));
        if (enabled_) {
            auto now = std::chrono::steady_clock::now();
            char line[160];
            if (stack.size() <= 3 && recent_ < 2000) {
                recent_++;
                double sec = std::chrono::duration<double>(now - start_).count();
                std::snprintf(line, sizeof(line), "[flight] @%.1fs T%zux %s\n", sec, (std::size_t)std::hash<std::thread::id>{}(key) % 100000u, name);
                std::fwrite(line, 1, std::strlen(line), stderr);
                std::fflush(stderr);
            }
            maybePrintSummary(now);
        }
    }

    void leave(const char* name) {
        if (!enabled_)
            return;
        auto key = std::this_thread::get_id();
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = stacks_.find(key);
        if (it == stacks_.end())
            return;
        auto& stack = it->second;
        if (stack.empty())
            return;
        // Pop this frame; tolerate mismatches by dropping the top.
        stack.pop_back();
        if (stack.empty())
            stacks_.erase(it);
    }

    void summary(const char* tag) {
        if (!enabled_)
            return;
        printSummary(tag);
    }

  private:
    Tracker() {
        const char* v = std::getenv("APS5_TRACE_FLIGHT");
        enabled_ = v && std::string(v) != "0";
        start_ = std::chrono::steady_clock::now();
        lastPrint_ = start_;
        if (enabled_)
            watcher_ = std::thread([this] { runWatcher(); });
    }

    void runWatcher() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(15));
            std::lock_guard<std::mutex> lock(mutex_);
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastPrint_).count() >= 15) {
                double sec = std::chrono::duration<double>(now - start_).count();
                if (sec > 58.0 && !finalPrinted_) {
                    finalPrinted_ = true;
                    printLocked(58.0);
                }
                maybePrintSummaryLocked(now);
            }
        }
    }

    void maybePrintSummary(std::chrono::steady_clock::time_point now) {
        maybePrintSummaryLocked(now);
    }

    void maybePrintSummaryLocked(std::chrono::steady_clock::time_point now) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastPrint_).count();
        if (elapsed < 15 || finalPrinted_)
            return;
        double sec = std::chrono::duration<double>(now - start_).count();
        if (sec > 58.0) {
            finalPrinted_ = true;
            printLocked(sec);
        }
        lastPrint_ = now;
    }

    void printSummary(const char* tag) {
        std::lock_guard<std::mutex> lock(mutex_);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
        (void)tag;
        printLocked(sec);
    }

    void printLocked(double sec) {
        char head[64];
        std::snprintf(head, sizeof(head), "[flight] summary @%.0fs: %zu threads in flight\n", sec, stacks_.size());
        std::fwrite(head, 1, std::strlen(head), stderr);
        for (auto& [key, stack] : stacks_) {
            // Outermost (index 0) is the current import call the thread sits in.
            const auto& outer = stack.front();
            double dwell = std::chrono::duration<double>(std::chrono::steady_clock::now() - outer.entered).count();
            char line[256];
            std::snprintf(line, sizeof(line), "  T%zux depth=%d dwell=%.1fs  %s\n",
                          (std::size_t)std::hash<std::thread::id>{}(key) % 100000u, outer.depth + 1, dwell, outer.name.c_str());
            std::fwrite(line, 1, std::strlen(line), stderr);
        }
        std::fflush(stderr);
    }

    std::mutex mutex_;
    std::unordered_map<std::thread::id, std::vector<Entry>> stacks_;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> finalPrinted_{false};
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point lastPrint_;
    std::thread watcher_;
    int recent_ = 0;
};

inline void enter(const char* name) {
    Tracker::instance().enter(name);
}

inline void leave(const char* name) {
    Tracker::instance().leave(name);
}

inline void summary(const char* tag) {
    Tracker::instance().summary(tag);
}

// RAII guard: mark this thread as inside the named host call on entry,
// clear it on exit (including exception unwind).
class Scope {
  public:
    explicit Scope(const char* name) : name_(name) {
        enter(name);
    }
    ~Scope() {
        leave(name_);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

  private:
    const char* name_;
};

} // namespace Aps5Flight
