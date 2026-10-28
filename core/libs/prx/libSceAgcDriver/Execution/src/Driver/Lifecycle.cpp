#include "prx/libSceAgcDriver/Execution/include/MemoryStats.hpp"
#include "prx/libSceAgcDriver/Execution/include/AsyncLog.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineJournal.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/WorkerSampler.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ParallelCompare.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineLibrary.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "ShaderDiskCache.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#endif

namespace AgcDriver::DriverDetail {

namespace {

#if defined(__linux__)
// A SIGSEGV or SIGBUS that no handler takes ends the process, often without a kept core: report the faulting
// address, the instruction and a backtrace first. Earlier handlers (write watch, the guest's exception handlers)
// run first and keep their behaviour; this only speaks when the fault would be fatal anyway.
struct sigaction previousSegv{}, previousBus{};

void describeAddress(std::uintptr_t address) {
    Dl_info where{};
    if (address == 0 || dladdr(reinterpret_cast<void*>(address), &where) == 0 || where.dli_fname == nullptr) {
        std::fprintf(stderr, "0x%llx", static_cast<unsigned long long>(address));
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(where.dli_fbase);
    std::fprintf(stderr, "0x%llx %s+0x%llx %s", static_cast<unsigned long long>(address), where.dli_fname, static_cast<unsigned long long>(address - base), where.dli_sname != nullptr ? where.dli_sname : "");
}

void fatalFault(int signal, siginfo_t* info, void* context) {
    const auto& previous = signal == SIGBUS ? previousBus : previousSegv;
    if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
        previous.sa_sigaction(signal, info, context);
        return;
    }
    if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN && previous.sa_handler != nullptr) {
        previous.sa_handler(signal);
        return;
    }
    const auto* frame = static_cast<const ucontext_t*>(context);
    std::fprintf(stderr, "[fault] signal %d code %d at address 0x%llx, thread %ld, instruction ", signal, info->si_code, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(info->si_addr)), static_cast<long>(gettid()));
    describeAddress(frame != nullptr ? static_cast<std::uintptr_t>(frame->uc_mcontext.gregs[REG_RIP]) : 0);
    std::fprintf(stderr, "\n");
    void* frames[48];
    const int count = backtrace(frames, 48);
    for (int index = 0; index < count; ++index) {
        std::fprintf(stderr, "[fault]   #%d ", index);
        describeAddress(reinterpret_cast<std::uintptr_t>(frames[index]));
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
    ::signal(signal, SIG_DFL);
}

void installFatalFaultReport() {
    struct sigaction action{};
    action.sa_sigaction = &fatalFault;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, &previousSegv);
    sigaction(SIGBUS, &action, &previousBus);
}
#endif

constexpr std::uintptr_t InlineImportStackBytes = 256 * 1024;

std::atomic<std::uint64_t> gpuMapImportThreads{0};

void PrintFailure(std::exception_ptr error) {
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& cause) {
        std::fprintf(stderr, "[gpu] driver failure: %s\n", cause.what());
    } catch (...) {
        std::fprintf(stderr, "[gpu] driver failure with a non-standard exception\n");
    }
    FlushAsyncLog_nid_no_patch();
    std::fflush(stderr);
}

constexpr std::uint64_t DriverMemoryAddress = 0xfe0000000ull + GuestArena::GuestAddressOffset;
constexpr std::size_t DriverMemoryBytes = 2u << 20u;
constexpr std::uint64_t FsTableAddress = DriverMemoryAddress + 0x40000ull;
constexpr std::uint64_t FsTableSlotBytes = 16u;
constexpr std::uint64_t FsTableSlots = 6u;
constexpr std::uint64_t FsRoutineRecords = 8u;
constexpr std::uint64_t FsRoutineRecordBytes = 16u;
constexpr std::uint64_t FsRoutineTableBytes = FsRoutineRecords * FsRoutineRecordBytes;
constexpr std::uint64_t FsRoutineTableAddress = FsTableAddress + FsTableSlots * FsTableSlotBytes;
constexpr std::uint64_t FsRoutineAddress = FsRoutineTableAddress + FsRoutineTableBytes;
constexpr std::uint32_t ScalarSetpcB64LinkS14 = 0xbe80200eu;
constexpr std::uint32_t ScalarEndProgram = 0xbf810000u;
constexpr std::uint64_t FsTableDword3 = 0x5204u;

void MapDriverMemory() {
    void* mapped;
#ifdef _WIN32
    mapped = reinterpret_cast<void*>(DriverMemoryAddress);
    GuestArena::GuestArenaCommit_nid_postfix(mapped, DriverMemoryBytes, PAGE_READWRITE, DriverMemoryBytes);
#else
    mapped = mmap(reinterpret_cast<void*>(DriverMemoryAddress), DriverMemoryBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mapped == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "map the AGC driver memory");
#endif
    if (reinterpret_cast<std::uint64_t>(mapped) != DriverMemoryAddress) throw std::runtime_error("the AGC driver memory was not mapped at its fixed address");
    auto* routineTable = reinterpret_cast<std::uint64_t*>(FsRoutineTableAddress);
    for (std::uint64_t record = 0; record < FsRoutineRecords; ++record) {
        routineTable[record * 2u] = FsRoutineAddress;
        routineTable[record * 2u + 1u] = 0u;
    }
    auto* routine = reinterpret_cast<std::uint32_t*>(FsRoutineAddress);
    routine[0] = ScalarSetpcB64LinkS14;
    routine[1] = ScalarEndProgram;
    auto* slot = reinterpret_cast<std::uint64_t*>(FsTableAddress);
    slot[0] = (FsRoutineTableAddress & 0xffffffffffffull) | 16ull << 48u;
    slot[1] = FsTableDword3 << 32u | FsRoutineRecords;
    GuestAllocations::Mutation mutation;
    mutation.Add(mapped, DriverMemoryBytes, true, true, true);
}

bool StackHoldsImport() {
#ifdef _WIN32
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
#else
    struct Bounds {
        std::uintptr_t low = 0;
        std::uintptr_t high = 0;
        Bounds() {
            pthread_attr_t attributes;
            if (pthread_getattr_np(pthread_self(), &attributes) != 0) return;
            void* base = nullptr;
            std::size_t size = 0;
            if (pthread_attr_getstack(&attributes, &base, &size) == 0 && base != nullptr) {
                low = reinterpret_cast<std::uintptr_t>(base);
                high = low + size;
            }
            pthread_attr_destroy(&attributes);
        }
    };
    thread_local const Bounds bounds;
    const auto low = bounds.low;
    const auto high = bounds.high;
#endif
    const auto frame = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    return frame >= low && frame < high && frame - low >= InlineImportStackBytes;
}

}

std::uint64_t Driver::GpuMapImportThreads() {
    return gpuMapImportThreads.load(std::memory_order_relaxed);
}

Driver& Driver::Get() {
    static Driver driver;
    return driver;
}

Driver::~Driver() {
    GuestArena::GuestArenaSetHostWriteHook_nid_no_patch(nullptr);
    GuestAllocations::GuestAllocationsSetGpuMapObserver_nid_postfix(nullptr);
    stop();
}

void Driver::Shutdown() {
    stop();
    std::lock_guard lock(mutex);
    rethrowFailure();
}

void Driver::stop() {
    require(!onWorkerThread(), "worker cannot stop itself");
    std::lock_guard shutdownLock(shutdownMutex);
    if (stopped) return;
    MemoryStats::Stop();
    LibcRequestShutdown_nid_postfix();
    {
        std::lock_guard lock(mutex);
        stopping = true;
        eopFlips.clear();
        for (auto& [queue, worker] : workers) {
            worker.pending.clear();
            worker.unfinishedWrites.clear();
            worker.queued.store(0, std::memory_order_release);
        }
    }
    changed.notify_all();
    for (auto& [queue, worker] : workers) {
        if (worker.thread.joinable()) worker.thread.join();
    }
    drawAhead.reset();
    DrawPipeline::Queue0().Stop();
    StopWorkerSampler();
    CloseRegistrationPreparation();
    Graphics::StopPipelineLibraryWorker();
    std::lock_guard gpuLock(GuestMemory::GpuMutex());
    Graphics::ReportHostImports();
    device.Reset();
    replacedDevices.clear();
    Graphics::FlushPipelineJournal();
    Graphics::ShutdownGuestBufferWorkers();
    Graphics::StopCompareHelpers();
    ShaderRecompiler::ShaderDiskCache::Shutdown();
    PerformanceTrace::StopOutput();
    FlushAsyncLogAtEnd_nid_no_patch();
    StopProfileOutput_nid_no_patch();
    stopped = true;
}

Driver::Driver() {
    InstallAsyncLog_nid_no_patch();
#if defined(__linux__)
    static const bool faultReport = (installFatalFaultReport(), true);
    static_cast<void>(faultReport);
#endif
    static const bool driverMemory = (MapDriverMemory(), true);
    static_cast<void>(driverMemory);
    LibcSetExitDrain_nid_no_patch([] {
        const auto current = Driver::Get().device.Load();
        if (current) current->SavePipelineCache();
    });
    static_cast<void>(GuestMemory::TrackerGeneration());
    ShaderMemory::SetWaitedMsProvider(&Graphics::Recorder::ThreadWaitedMs);
    try {
        LibcRegisterShutdown_nid_postfix([] { Driver::Get().Shutdown(); });
    } catch (...) {
        stop();
        throw;
    }
    GuestArena::GuestArenaSetHostWriteHook_nid_no_patch(&GuestMemory::PrepareHostWrite);
    GuestAllocations::GuestAllocationsSetGpuMapObserver_nid_postfix([](const GuestAllocations::Mapped& ranges, std::uint64_t generation) {
        const auto import = [&] {
            try {
                const auto current = Driver::Get().device.Load();
                if (current == nullptr || current->ImportGuestMemory(ranges, generation, false)) return;
                std::lock_guard lock(GuestMemory::GpuMutex());
                if (Driver::Get().device.Load() == current) current->ImportGuestMemory(ranges, generation, true);
            } catch (const std::exception& error) {
                std::fprintf(stderr, "[gpu] import of GPU memory at its mapping failed: %s\n", error.what());
            }
        };
        if (GuestMemory::GpuMutex().HeldByThisThread() || StackHoldsImport()) {
            import();
            return;
        }
        std::thread host;
        try {
            host = std::thread(import);
        } catch (const std::system_error& error) {
            std::fprintf(stderr, "[gpu] import of GPU memory at its mapping failed: no host thread to run it on: %s\n", error.what());
            return;
        }
        gpuMapImportThreads.fetch_add(1, std::memory_order_relaxed);
        host.join();
    });
}

void Driver::WaitIdle() {
    require(!onWorkerThread(), "worker cannot wait for itself");
    std::unique_lock lock(mutex);
    const auto target = accepted;
    ++idleWaiters;
    changed.wait(lock, [&] { return failure != nullptr || stopping || completed >= target; });
    --idleWaiters;
    rethrowFailure();
    checkStopping();
}

void Driver::CheckFailure() {
    if (failed.load(std::memory_order_acquire)) {
        std::lock_guard lock(mutex);
        rethrowFailure();
    }
    checkStopping();
}

void Driver::ReportFailure(std::exception_ptr error) {
    require(error != nullptr, "null asynchronous failure");
    bool first = false;
    {
        std::lock_guard lock(mutex);
        first = !failure;
        if (first) failure = error;
        failed.store(true, std::memory_order_release);
        for (const auto& [handle, output] : outputs) output->Fail(failure);
        for (auto& [queue, worker] : workers) {
            for (const auto& item : worker.pending) {
                for (const auto& [offset, flip] : item.flips) flip->Fail(failure);
            }
            worker.pending.clear();
            worker.unfinishedWrites.clear();
            worker.queued.store(0, std::memory_order_release);
        }
        if (first) PrintFailure(error);
    }
    changed.notify_all();
}

void Driver::rethrowFailure() const {
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

void Driver::checkStopping() const {
    if (stopping.load(std::memory_order_acquire) || shutdownToken.stop_requested()) throw ProcessShutdown{};
}

bool& Driver::onWorkerThread() {
    static thread_local bool worker = false;
    return worker;
}

}

namespace AgcDriver {

void FlushAsyncLog_nid_no_patch() {
    AsyncLog::Get().Flush();
}

std::unique_lock<std::mutex> HoldAsyncLog() {
    return AsyncLog::Get().Hold();
}

}
