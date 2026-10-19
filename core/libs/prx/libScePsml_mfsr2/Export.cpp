// A26: stub for libScePsml_mfsr2 (PS5 ML upscaler, sysmodule 0x10B).

// field1 = module index 39 = 'n'): the 3 IAT slots the engine calls plus
// 4 more the relinker must resolve at link time.
// A26a: every export logs its args and returns 0x80020016 ("upscaler not
// supported"), the most likely branch into the engine's non-PSSR fallback.
#include <cstdint>
#include "prx/libc/include/General.hpp"

namespace {

constexpr std::int32_t kMfsr2NotSupported = static_cast<std::int32_t>(0x80020016);

std::int32_t mfsr2Stub(const char* nid, void* a0, void* a1, void* a2, void* a3) {
    APS5_LOG_OUT("mfsr2-stub: %s(a0=%p, a1=%p, a2=%p, a3=%p) -> 0x80020016",
                 nid, a0, a1, a2, a3);
    return kMfsr2NotSupported;
}

}

extern "C" {

// APS5_EXPORT(NID, func) aliases "NID_nid_no_patch_cut" -> func; the NID
// patcher strips the marker so the PE export name is exactly the guest NID.

APS5_EXPORT("o+NM86gEwFE", mfsr2_e0);
std::int32_t APS5_VABI mfsr2_e0(void* a0, void* a1, void* a2, void* a3) {
    return mfsr2Stub("o+NM86gEwFE", a0, a1, a2, a3);
}

APS5_EXPORT("1ic5q-kdOsc", mfsr2_e1);
std::int32_t APS5_VABI mfsr2_e1(void* a0, void* a1, void* a2, void* a3) {
    return mfsr2Stub("1ic5q-kdOsc", a0, a1, a2, a3);
}

APS5_EXPORT("gMduXCLYrNg", mfsr2_e2);
std::int32_t APS5_VABI mfsr2_e2(void* a0, void* a1, void* a2, void* a3) {
    return mfsr2Stub("gMduXCLYrNg", a0, a1, a2, a3);
}

APS5_EXPORT("0GAw7SmkwII", mfsr2_e3);
std::int32_t APS5_VABI mfsr2_e3(void* a0, void* a1, void* a2, void* a3) {
    return mfsr2Stub("0GAw7SmkwII", a0, a1, a2, a3);
}

APS5_EXPORT("5z2gBlqxJ+0", mfsr2_e4);
std::int32_t APS5_VABI mfsr2_e4(void* a0, void* a1, void* a2, void* a3) {
    return mfsr2Stub("5z2gBlqxJ+0", a0, a1, a2, a3);
}

APS5_EXPORT("m9JLPc3wOQw", mfsr2_e5);
std::int32_t APS5_VABI mfsr2_e5(void* a0, void* a1, void* a2, void* a3) {
    return mfsr2Stub("m9JLPc3wOQw", a0, a1, a2, a3);
}

APS5_EXPORT("Kyy1baXgaVU", mfsr2_e6);
std::int32_t APS5_VABI mfsr2_e6(void* a0, void* a1, void* a2, void* a3) {
    return mfsr2Stub("Kyy1baXgaVU", a0, a1, a2, a3);
}

}
