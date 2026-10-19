#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
const bool kFlipTrace = [] { return std::getenv("APS5_TRACE_FLIP") != nullptr; }();
unsigned flipTid() {
#ifdef _WIN32
    return ::GetCurrentThreadId();
#else
    return 0;
#endif
}
}

extern "C" {

int APS5_VABI sceAgcDriverSubmitAcb(uint32_t queue, const Packet* packet) {
    if (queue < 0x20 || queue >= 0x58) {
        throw std::runtime_error(std::string(__func__) + ": unsupported compute queue");
    }
    if (kFlipTrace) std::fprintf(stderr, "[flip] submit-api sceAgcDriverSubmitAcb queue=0x%x dwords=%u tid=0x%x\n", queue, packet ? packet->dw_num : 0, flipTid());
    AgcDriver::Submit(packet, queue);
    return 0;
}

int APS5_VABI sceAgcDriverSubmitMultiAcbs(uint32_t queue, uint32_t* const* acbs, const uint32_t* sizes_in_dwords, uint32_t count) {
    if (count == 0) return 0;
    if (queue < 0x20 || queue >= 0x58) {
        throw std::runtime_error(std::string(__func__) + ": unsupported compute queue");
    }
    return AgcDriver::SubmitMany(acbs, sizes_in_dwords, count, queue);
}

}
