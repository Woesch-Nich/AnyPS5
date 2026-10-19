#include "prx/libSceAgcDriver/Eq/include/Event.hpp"

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/Equeue/Equeue.hpp"
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr int16_t EvfiltGraphicsCore = -14;
const bool kFlipTrace = [] { return std::getenv("APS5_TRACE_FLIP") != nullptr; }();
unsigned flipTid() {
#ifdef _WIN32
    return ::GetCurrentThreadId();
#else
    return 0;
#endif
}

struct Registration {
    KernelEqueue eq;
    int id;
};

std::mutex g_mutex;
std::vector<Registration> g_registrations;

}

void AgcDriverDeliverEopInterrupt(std::uint32_t queue) {
    std::vector<Registration> registrations;
    {
        std::lock_guard lock(g_mutex);
        registrations = g_registrations;
    }
    std::size_t matches = 0;
    for (const auto& registration : registrations) {
        if (registration.id == static_cast<int>(queue)) {
            if (kFlipTrace) std::fprintf(stderr, "[flip] eop-post queue=%u eq=0x%x tid=0x%x\n", queue, registration.eq, flipTid());
            EqueueTriggerEvent_nid_postfix(registration.eq, static_cast<uintptr_t>(registration.id), EvfiltGraphicsCore, nullptr);
            ++matches;
        }
    }
    if (kFlipTrace) std::fprintf(stderr, "[flip] eop-post queue=%u matched=%d regs=%zu tid=0x%x\n", queue, matches, registrations.size(), flipTid());
}

extern "C" {

int APS5_VABI sceAgcDriverAddEqEvent(KernelEqueue eq, int id, void* udata) {
    if (eq == 0 || !EqueuePin_nid_postfix(eq)) {
        throw std::runtime_error(std::string(__func__) + ": invalid event queue");
    }
    KernelEqueueEvent event{};
    event.event.ident = static_cast<uintptr_t>(id);
    event.event.filter = EvfiltGraphicsCore;
    event.event.flags = EV_ADD | EV_CLEAR;
    event.event.udata = udata;
    event.filter.triggerFunc = [](KernelEqueueEvent* e, void*) {
        e->event.data = e->triggered ? e->event.data + 1 : 1;
        e->triggered = true;
    };
    event.filter.resetFunc = [](KernelEqueueEvent* e) {
        e->triggered = false;
        e->event.data = 0;
    };
    const int result = EqueueAddEvent_nid_postfix(eq, event);
    if (result != EQUEUE_OK) {
        throw std::runtime_error(std::string(__func__) + ": event queue rejected the event");
    }
    if (kFlipTrace) std::fprintf(stderr, "[flip] eq-register eq=0x%x id=%d udata=%p tid=0x%x\n", eq, id, udata, flipTid());
    std::lock_guard lock(g_mutex);
    if (std::none_of(g_registrations.begin(), g_registrations.end(), [&](const Registration& r) { return r.eq == eq && r.id == id; })) {
        g_registrations.push_back({eq, id});
    }
    return 0;
}

int APS5_VABI sceAgcDriverDeleteEqEvent(KernelEqueue eq, int id) {
    {
        std::lock_guard lock(g_mutex);
        std::erase_if(g_registrations, [&](const Registration& r) { return r.eq == eq && r.id == id; });
    }
    if (EqueueDeleteEvent_nid_postfix(eq, static_cast<uintptr_t>(id), EvfiltGraphicsCore) != EQUEUE_OK) {
        throw std::runtime_error(std::string(__func__) + ": event is not registered");
    }
    return 0;
}

}
