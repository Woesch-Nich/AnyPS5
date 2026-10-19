// a214 (#A214-answer): unit tests for the whole-range (64 MB chunk) placeholder Map() behind
// APS5_MAP_WHOLE=1. Covers, as ordered by Doc: partial unmap / partial protect / remap after a
// whole map. Sub-chunk unmap is the documented chunk-granularity limitation and must throw.
#include "prx/libc/include/WindowsMappings.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <stdexcept>
#include <windows.h>

using GuestArena::WindowsMappings;

static void Require(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        std::exit(1);
    }
}

int main() {
    // whole mode is captured once (function-local static) on the first Map() — set before use.
    _putenv_s("APS5_MAP_WHOLE", "1");
    const std::size_t chunk = 64ull * 1024 * 1024;   // 64 MB = the whole-mode step
    const std::size_t total = 5 * 32ull * 1024 * 1024;  // 160 MB = 2 full chunks + 32 MB partial chunk
    auto& mappings = WindowsMappings::Get();
    void* base = mappings.Reserve(nullptr, total);
    Require(base != nullptr, "Reserve placeholder");
    const auto a = reinterpret_cast<std::uintptr_t>(base);

    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE,
        static_cast<DWORD>(total >> 32), static_cast<DWORD>(total & 0xffffffff), L"aps5-wm-whole-test");
    Require(section != nullptr, "CreateFileMappingW");

    // 1) Whole map across 3 chunks (64+64+32 MB) must succeed.
    mappings.Map(base, total, section, 0, PAGE_EXECUTE_READWRITE);

    // 2) Writes + reads across every chunk boundary.
    memset(reinterpret_cast<char*>(a), 0xA1, total);
    for (std::size_t off = 0; off < total; off += 16ull * 1024 * 1024) {
        Require(*reinterpret_cast<const unsigned char*>(a + off) == 0xA1, "write/read back after whole map");
    }

    // 3) Partial SetProtection of a sub-range must not throw (chunk-granularity bookkeeping).
    mappings.SetProtection(a + 32ull * 1024 * 1024, 16ull * 1024 * 1024, PAGE_READWRITE);

    // 2b) Host read-write alias of a SUB-CHUNK range mid-view (the a214 on1 regression:
    // MapAlias 0x1120400000+0x400000 died with 'not one contiguous run of views' in chunk mode).
    {
        void* alias = mappings.MapAlias(a + 16ull * 1024 * 1024, 4ull * 1024 * 1024);
        Require(alias != nullptr, "MapAlias sub-chunk range");
        memset(alias, 0x3D, 4ull * 1024 * 1024);
        Require(*reinterpret_cast<const unsigned char*>(a + 16ull * 1024 * 1024) == 0x3D, "alias write visible in guest map");
        mappings.UnmapAlias(alias);
    }

    // 4) Chunk-ALIGNED partial unmap: release the first full 64 MB chunk, leave the rest mapped.
    mappings.Reset(base, chunk);
    {
        MEMORY_BASIC_INFORMATION mbi{};
        Require(VirtualQuery(base, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_RESERVE,
                "first chunk back to placeholder");
        memset(reinterpret_cast<char*>(a + chunk), 0x5C, total - chunk);
        Require(*reinterpret_cast<const unsigned char*>(a + chunk + (total - chunk) - 1) == 0x5C,
                "remaining chunks still mapped");
    }

    // 5) Sub-CHUNK partial unmap: documented chunk-granularity limitation — must throw.
    bool threw = false;
    try {
        mappings.Reset(reinterpret_cast<void*>(a + chunk + 32ull * 1024 * 1024), 32ull * 1024 * 1024);
    } catch (const std::runtime_error& e) {
        threw = true;
        Require(std::string(e.what()).find("cannot release part of a host allocation") != std::string::npos,
                "expected 'cannot release part of a host allocation'");
    }
    Require(threw, "sub-chunk reset must throw");

    // 6) Remap the released chunk (placeholder -> map again) must work, and be writable.
    mappings.Map(base, chunk, section, 0, PAGE_EXECUTE_READWRITE);
    memset(base, 0x77, chunk);
    Require(*reinterpret_cast<const unsigned char*>(base) == 0x77, "remap after unmap");

    // 7) Release the rest (chunk-aligned: full 64 MB chunk + 32 MB tail chunk).
    mappings.Reset(reinterpret_cast<void*>(a + chunk), chunk);
    mappings.Reset(reinterpret_cast<void*>(a + 2 * chunk), total - 2 * chunk);

    CloseHandle(section);
    std::printf("windows_mappings_whole_map PASS\n");
    return 0;
}
