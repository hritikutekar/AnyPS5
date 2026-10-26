#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/TracyZone.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libc/include/CpuTopology.hpp"
#include "prx/libc/include/general/ThreadName.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <vector>
#ifdef __linux__
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
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

namespace {

std::array<std::atomic<long>, 3> criticalThreads{};

#ifdef __linux__
struct Placement {
    std::uint64_t process = 0;
    std::array<std::uint64_t, 3> critical{};
    std::uint64_t rest = 0;
};

// The physical cores (each a mask of its logical CPUs) of the process mask, in order of their lowest CPU.
std::vector<std::uint64_t> physicalCores(std::uint64_t process) {
    std::map<unsigned, std::uint64_t> byFirst;
    for (unsigned cpu = 0; cpu < 64; ++cpu) {
        if (((process >> cpu) & 1u) == 0) continue;
        char path[96];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list", cpu);
        auto siblings = CpuTopology::MaskFromCpuList(path);
        if (siblings == 0) siblings = std::uint64_t{1} << cpu;
        siblings &= process;
        byFirst[static_cast<unsigned>(__builtin_ctzll(siblings))] = siblings;
    }
    std::vector<std::uint64_t> cores;
    for (const auto& [first, mask] : byFirst) cores.push_back(mask);
    return cores;
}

// The placement, computed once from the worker's mask before anything is pinned; process == 0 when
// pinning does not apply on this machine.
const Placement& placement() {
    static const Placement result = [] {
        Placement placed{};
        cpu_set_t set;
        CPU_ZERO(&set);
        if (sched_getaffinity(0, sizeof(set), &set) != 0) return placed;
        std::uint64_t process = 0;
        for (unsigned cpu = 0; cpu < 64; ++cpu)
            if (CPU_ISSET(cpu, &set)) process |= std::uint64_t{1} << cpu;
        const auto cores = physicalCores(process);
        if (CpuTopology::Get().hybrid || cores.size() < 6) {
            std::fprintf(stderr, "[affinity] thread pinning off: %zu physical cores%s\n", cores.size(), CpuTopology::Get().hybrid ? ", hybrid CPU" : "");
            return placed;
        }
        std::uint64_t taken = 0;
        for (std::size_t i = 0; i < 3; ++i) {
            const auto core = cores[cores.size() * (i + 1) / 4];
            placed.critical[i] = core & (~core + 1);
            taken |= core;
        }
        placed.process = process;
        placed.rest = process & ~taken;
        std::fprintf(stderr, "[affinity] thread pinning: queue-0 worker 0x%llx, committer 0x%llx, preparer 0x%llx, other threads 0x%llx (process 0x%llx, %zu physical cores)\n", static_cast<unsigned long long>(placed.critical[0]), static_cast<unsigned long long>(placed.critical[1]), static_cast<unsigned long long>(placed.critical[2]), static_cast<unsigned long long>(placed.rest), static_cast<unsigned long long>(process), cores.size());
        return placed;
    }();
    return result;
}

bool setThreadMask(long tid, std::uint64_t mask) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (unsigned cpu = 0; cpu < 64; ++cpu)
        if ((mask >> cpu) & 1u) CPU_SET(cpu, &set);
    return sched_setaffinity(static_cast<pid_t>(tid), sizeof(set), &set) == 0;
}

// Every thread of the process: the critical ones on their CPUs when pinning, everything on the process
// mask when not. Returns the threads placed.
unsigned placeThreads(const Placement& placed, bool pinning) {
    DIR* tasks = opendir("/proc/self/task");
    if (tasks == nullptr) return 0;
    std::array<long, 3> critical{};
    for (std::size_t i = 0; i < critical.size(); ++i) critical[i] = criticalThreads[i].load(std::memory_order_acquire);
    unsigned placedThreads = 0;
    while (const dirent* entry = readdir(tasks)) {
        char* end = nullptr;
        const long tid = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0') continue;
        auto mask = pinning ? placed.rest : placed.process;
        for (std::size_t i = 0; i < critical.size(); ++i) {
            if (pinning && critical[i] == tid) mask = placed.critical[i];
        }
        if (setThreadMask(tid, mask)) ++placedThreads;
    }
    closedir(tasks);
    return placedThreads;
}
#endif

}

void NoteCriticalThread(CriticalThread role) {
#ifdef __linux__
    criticalThreads[static_cast<std::size_t>(role)].store(static_cast<long>(gettid()), std::memory_order_release);
#else
    static_cast<void>(role);
#endif
}

void FollowThreadPinning() {
#ifdef __linux__
    const auto& placed = placement();
    if (placed.process == 0) return;
    static bool applied = false;
    static bool pinned = false;
    static std::chrono::steady_clock::time_point lastSweep{};
    const bool pinning = !Graphics::RuntimeSwitches::Set(Graphics::RuntimeSwitch::NoThreadPinning);
    const auto now = std::chrono::steady_clock::now();
    if (applied && pinning == pinned && (!pinning || now - lastSweep < std::chrono::seconds(1))) return;
    const auto threads = placeThreads(placed, pinning);
    if (!applied || pinning != pinned) std::fprintf(stderr, "[affinity] thread pinning %s (%u threads placed)\n", pinning ? "on" : "off", threads);
    applied = true;
    pinned = pinning;
    lastSweep = now;
#endif
}

}

