#ifndef CORE_LIBS_PRX_LIBKERNEL_MODULE_INCLUDE_GUESTIMAGEBASES_HPP
#define CORE_LIBS_PRX_LIBKERNEL_MODULE_INCLUDE_GUESTIMAGEBASES_HPP

#include <cstdint>

// Tracks the allocation bases of guest modules (PRX / .guest.prx loaded at
// runtime via dlopen) so that DirectMemory can distinguish "a guest module's
// own image" from "a truly foreign host image" when the game requests memory
// protection. The main executable is handled separately (RegisterMainImage).
namespace GuestImages {

// True when `base` is the allocation base of a currently loaded guest module.
bool ContainsBase(const void* base);

// `base` + SizeOfImage for a loaded guest module, or 0 when `base` is not a
// known guest-module base. DirectMemory uses this to confirm the protected
// range stays inside the module image.
std::uintptr_t EndForBase(const void* base);

} // namespace GuestImages

#endif
