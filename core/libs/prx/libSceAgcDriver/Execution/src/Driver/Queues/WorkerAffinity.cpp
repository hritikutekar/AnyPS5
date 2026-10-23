#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/TracyZone.hpp"
#include "prx/libc/include/CpuTopology.hpp"
#include "prx/libc/include/general/ThreadName.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef __linux__
#include <pthread.h>
#endif

namespace AgcDriver::DriverDetail {

std::uint64_t WorkerAffinityMask() {
    static const std::uint64_t mask = [] {
        if (std::getenv("APS5_NO_WORKER_AFFINITY") != nullptr) return std::uint64_t{0};
        const auto requested = CpuTopology::MaskFromEnvironment("APS5_WORKER_AFFINITY_MASK");
        if (requested != 0) return requested;
#ifndef __linux__
        if (std::getenv("APS5_WORKER_AFFINITY") == nullptr) return std::uint64_t{0};
#endif
        const auto& layout = CpuTopology::Get();
        return layout.hybrid ? layout.performant : std::uint64_t{0};
    }();
    return mask;
}

void NameCurrentThread(const char* role) {
#ifdef __linux__
    char name[16];
    unsigned queue = 0;
    if (std::sscanf(role, "queue worker 0x%x", &queue) == 1) std::snprintf(name, sizeof(name), "agc q0x%x", queue);
    else std::snprintf(name, sizeof(name), "agc %s", role);
    static_cast<void>(pthread_setname_np(pthread_self(), name));
#else
    static_cast<void>(role);
#endif
}

void PinWorkerThread(const char* role) {
    TracyZone::NameThread(role);
#ifdef _WIN32
    const HRESULT nameResult = ThreadName::SetCurrent(role);
    if (FAILED(nameResult)) std::fprintf(stderr, "[thread-name] could not name %s (0x%08lx)\n", role, static_cast<unsigned long>(nameResult));
#else
    NameCurrentThread(role);
#endif
    const auto mask = WorkerAffinityMask();
    if (mask == 0) return;
    static std::once_flag summary;
    std::call_once(summary, [mask] {
        const auto& layout = CpuTopology::Get();
        std::fprintf(stderr, "[affinity] queue workers and presenter -> 0x%llx (hybrid=%d efficient=0x%llx performant=0x%llx process=0x%llx)\n", static_cast<unsigned long long>(mask), layout.hybrid ? 1 : 0, static_cast<unsigned long long>(layout.efficient), static_cast<unsigned long long>(layout.performant), static_cast<unsigned long long>(layout.process));
    });
    CpuTopology::PinTraced(role, nullptr, mask);
}

}
