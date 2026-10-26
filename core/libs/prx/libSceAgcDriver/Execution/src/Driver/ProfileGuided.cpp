#include "prx/libSceAgcDriver/Execution/include/Driver/ProfileGuided.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>

// Defined only when the library is linked with -fprofile-generate.
extern "C" void __gcov_dump() __attribute__((weak));

namespace AgcDriver::DriverDetail {

void NotePgoPresent(std::uint64_t present) {
    if (__gcov_dump == nullptr) return;
    static const std::uint64_t dumpAt = [] { const char* text = std::getenv("APS5_PGO_DUMP_AT"); return text != nullptr ? std::strtoull(text, nullptr, 10) : 2600ull; }();
    static std::atomic<bool> dumped{false};
    if (present < dumpAt || dumped.exchange(true)) return;
    __gcov_dump();
    std::fprintf(stderr, "[pgo] profile written at present %llu\n", static_cast<unsigned long long>(present));
}

}
