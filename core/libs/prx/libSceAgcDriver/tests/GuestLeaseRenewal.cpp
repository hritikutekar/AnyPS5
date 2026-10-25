#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>

// An unmap waits for the leases holding its range. A driver worker that keeps building (every
// address-based build leases every registered range, and consecutive builds overlap) must not keep the
// range pinned forever: leases taken after the unmap began leave the range out (best137/202/210 aborted
// after 60 s of such re-leasing). APS5_NO_PIN_DRAIN_EXCLUSION=1 restores the old behaviour, under which
// this test fails.

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool holds(const GuestAllocations::Lease& lease, std::uintptr_t address) {
    return std::any_of(lease.begin(), lease.end(), [&](const auto& range) { return range->address == address; });
}

void renewedLeasesDoNotPin() {
    alignas(64) static std::byte memory[128]{};
    const auto address = reinterpret_cast<std::uintptr_t>(memory);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(memory, 64, true, true, true);
        mutation.Add(memory + 64, 64, true, true, true);
    }
    std::atomic<bool> holding = false;
    std::atomic<bool> unmapStarted = false;
    std::atomic<bool> stop = false;
    std::atomic<bool> releasedAfterStart = false;
    std::exception_ptr failure;
    // The builder: overlapping leases, each held 2 ms past the next one's acquisition, for at most 3 s.
    std::thread builder([&] {
        try {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            auto previous = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
            require(holds(previous, address), "the first lease misses the registered range");
            holding = true;
            while (!stop && std::chrono::steady_clock::now() < until) {
                const bool started = unmapStarted.load();
                auto next = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
                if (started && holds(next, address)) releasedAfterStart = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                previous = std::move(next);
            }
        } catch (...) { failure = std::current_exception(); }
    });
    while (!holding) std::this_thread::yield();
    const auto start = std::chrono::steady_clock::now();
    bool unmapped = false;
    {
        GuestAllocations::Mutation mutation;
        unmapStarted = true;
        mutation.Unmap(memory, 64, [&](const void*, std::size_t, const void*, bool) { unmapped = true; });
    }
    const auto waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    stop = true;
    builder.join();
    {
        GuestAllocations::Mutation mutation;
        if (!unmapped) mutation.Remove(memory);
        mutation.Remove(memory + 64);
    }
    if (failure) std::rethrow_exception(failure);
    std::printf("unmap waited %.1f ms while the builder kept leasing\n", waited);
    require(unmapped, "the unmap did not run");
    std::printf("leases taken after the unmap began held the range: %s\n", releasedAfterStart.load() ? "yes" : "no");
    require(waited < 1000.0, "an unmap waited " + std::to_string(waited) + " ms for leases taken after it began");
}

}

int main() {
    try {
        renewedLeasesDoNotPin();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
    std::puts("ok");
    return 0;
}
