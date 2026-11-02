#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/WaitMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/SubmissionRing.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include <array>
#include <unordered_set>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif
#include <immintrin.h>
#include <algorithm>
#include <tuple>
#include <vector>

namespace AgcDriver::DriverDetail {

void PollSleep() {
#ifdef _WIN32
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer != nullptr) {
        LARGE_INTEGER due{};
        due.QuadPart = -2000;
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
#endif
    std::this_thread::sleep_for(std::chrono::microseconds(200));
}

int Driver::waitTimeoutMs() {
    static const int value = [] { const char* text = std::getenv("APS5_GPU_WAIT_TIMEOUT_MS"); return text ? std::atoi(text) : 1000; }();
    return value;
}

bool Driver::pollReapAll() {
    static const bool all = std::getenv("APS5_POLL_REAP") != nullptr;
    return all;
}

bool Driver::pollTryEach() {
    static const bool each = std::getenv("APS5_POLL_TRY_EACH") != nullptr;
    return each;
}

bool Driver::traceLateLabels() {
    static const bool trace = std::getenv("APS5_TRACE_LATE_LABELS") != nullptr;
    return trace;
}

std::vector<Driver::PendingWait>& Driver::pendingWaits() {
    struct WaitsTag {};
    return HostThreadLocal<std::vector<PendingWait>, WaitsTag>();
}

void Driver::notePendingWait(std::span<const std::uint32_t> packet, std::uint32_t queue, std::uint64_t received) {
    if (packet.size() > std::tuple_size_v<decltype(PendingWait::words)>) return;
    static const bool allQueues = std::getenv("APS5_LAND_WAITS_ALL_QUEUES") != nullptr;
    if (!allQueues && queue == 0) return;
    if (Pm4::WaitSatisfiedUnchecked(packet)) return;
    auto& waits = pendingWaits();
    for (const auto& wait : waits) {
        if (wait.count == packet.size() && std::equal(packet.begin(), packet.end(), wait.words.begin())) return;
    }
    PendingWait wait{};
    std::copy(packet.begin(), packet.end(), wait.words.begin());
    wait.count = packet.size();
    wait.received = received;
    waits.push_back(wait);
}

void Driver::landPendingWaits(std::uint32_t queue) {
    auto& waits = pendingWaits();
    if (waits.empty()) return;
    // CEILING EXPERIMENT ONLY (may read stale data; never a default): APS5_CEILING_NO_LAND_WAITS=1 drops the pending
    // cross-ring waits instead of waiting for the GPU to run the producer's label store before a guest read.
    static const bool ceilingNoLand = [] {
        const char* value = std::getenv("APS5_CEILING_NO_LAND_WAITS");
        return value != nullptr && *value == '1';
    }();
    if (ceilingNoLand) {
        waits.clear();
        return;
    }
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    struct LandStats {
        std::uint64_t lands = 0, waited = 0;
        double waitedMs = 0, maxMs = 0;
        std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
    };
    static thread_local LandStats stats;
    const PacketHistory none{};
    for (const auto& wait : waits) {
        const auto packet = std::span<const std::uint32_t>(wait.words).first(wait.count);
        ++stats.lands;
        if (Pm4::WaitSatisfiedUnchecked(packet)) continue;
        const auto started = std::chrono::steady_clock::now();
        waitMemory(packet, queue, none, wait.received, false, true);
        // Critical-path trace: a landed cross-ring wait waits for the GPU to run the producer's label store.
        if (auto* trace = PerformanceTrace::Recording() ? PerformanceTrace::Thread() : nullptr) {
            const auto previous = std::exchange(AgcDriver::TraceDepTag, 2ull << 60u);
            trace->Add("Driver.LandWait", nullptr, started, std::chrono::steady_clock::now());
            AgcDriver::TraceDepTag = previous;
        }
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        ++stats.waited;
        stats.waitedMs += ms;
        stats.maxMs = std::max(stats.maxMs, ms);
    }
    waits.clear();
    if (profile && std::chrono::steady_clock::now() - stats.reported > std::chrono::seconds(10)) {
        AgcDriver::ProfilePrint_nid_no_patch("[landwait] queue 0x%x (10 s): %llu waits landed before a guest read, %llu had to wait for the GPU: %.1f ms in total, max %.2f ms\n", queue, static_cast<unsigned long long>(stats.lands), static_cast<unsigned long long>(stats.waited), stats.waitedMs, stats.maxMs);
        stats = LandStats{};
    }
}

namespace {

// Diagnostic (APS5_PROFILE_DRAW): every ring's blocked-wait time so far and whether it is blocked now, so a queue-0
// wait that ends on ring R's label or store splits into "R was itself blocked" and "R was working" ([q-split]).
struct RingBlocked {
    std::atomic<std::int64_t> cumulativeNs{0};
    std::atomic<std::int64_t> sinceNs{0};
    std::atomic<std::uint32_t> producer{0xffffffffu};
};

std::array<RingBlocked, 256>& RingsBlocked() {
    static std::array<RingBlocked, 256> rings;
    return rings;
}

std::int64_t SteadyNs() {
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

namespace {
// CEILING EXPERIMENT ONLY (renders wrong on purpose; never a default): APS5_CEILING_NO_CROSS_WAIT=1 learns the addresses
// whose blocked waits were ended by ANOTHER ring's label or store, and from then on returns from an unsatisfied wait on
// such an address at once: the waiting ring records ahead of its producer (wrong GPU order, wrong data). It measures the
// most that breaking the cross-ring chain (record-ahead) could save.
bool CeilingNoCrossWait() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_CEILING_NO_CROSS_WAIT");
        const bool on = value != nullptr && *value == '1';
        if (on) std::fprintf(stderr, "[ceiling] APS5_CEILING_NO_CROSS_WAIT=1: cross-ring waits skipped once learned (rendering is WRONG on purpose)\n");
        return on;
    }();
    return enabled;
}
std::mutex& CrossWaitMutex() {
    static std::mutex mutex;
    return mutex;
}
std::unordered_set<std::uint64_t>& CrossWaitAddresses() {
    static auto* addresses = new std::unordered_set<std::uint64_t>;
    return *addresses;
}
std::atomic<std::uint64_t> crossWaitsSkipped{0};
}

void Driver::waitMemory(std::span<const std::uint32_t> packet, std::uint32_t queue, const PacketHistory& context, std::uint64_t received, bool heldAtSubmit, bool requireMemory) {

    auto start = std::chrono::steady_clock::now();
    auto lastDone = packetsDone.load();

    GuestMemory::TagGpuLockThread(queue);
    auto& outcomes = waitOutcomes();
    const bool wide = ((packet[0] >> 8u) & 0xffu) == 0x93u;
    const std::size_t awaitedBytes = Pm4::WaitAwaitedBytes(packet);
    const auto alignMask = awaitedBytes == 8 ? ~7ull : ~3ull;
    const std::uint64_t awaited = (packet[2] & static_cast<std::uint32_t>(alignMask)) | (static_cast<std::uint64_t>(packet[3]) << 32u);
    static const bool traceGpu = std::getenv("APS5_TRACE_GPU") != nullptr;
    if (traceGpu) std::fprintf(stderr, "[gpu] %.1f queue 0x%x waits 0x%llx == 0x%x (now 0x%x)\n", TraceMs(), queue, static_cast<unsigned long long>(awaited), packet[4],
                               *reinterpret_cast<const volatile std::uint32_t*>(awaited));
    struct WaitTrace {
        bool enabled; std::uint32_t queue; std::uint64_t address; std::chrono::steady_clock::time_point begin;
        ~WaitTrace() {
            if (!enabled) return;
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            if (ms >= 0.5) std::fprintf(stderr, "[gpu] %.1f queue 0x%x wait on 0x%llx done after %.1f ms\n", TraceMs(), queue, static_cast<unsigned long long>(address), ms);
        }
    } waitTrace{traceGpu, queue, awaited, std::chrono::steady_clock::now()};
    bool warned = false;

    struct EpochPoint {
        bool bump = true;
        ~EpochPoint() {
            if (bump) bumpEpoch(&EpochBumps::waits);
        }
    } epochPoint;
    GuestMemory::CheckRange(reinterpret_cast<const void*>(awaited), awaitedBytes, awaitedBytes);
    if (Pm4::WaitSatisfiedUnchecked(packet)) {
        ++outcomes.atEntry;
        return;
    }
    if (heldAtSubmit) return;
    if (std::uint32_t writer = queue; !requireMemory && storedSince(packet, awaited, awaitedBytes, received, &writer)) {
        if (writer != queue) notePendingWait(packet, queue, received);
        else epochPoint.bump = false;
        return;
    }

    static const bool labelShortcut = std::getenv("APS5_NO_LABEL_SHORTCUT") == nullptr;
    const bool labelTable = labelShortcut && !requireMemory;
    static const bool overlapSubmit = std::getenv("APS5_NO_WAIT_OVERLAP_SUBMIT") == nullptr;

    static const bool waitLock = std::getenv("APS5_WAIT_LOCK") != nullptr;

    std::uint64_t refusedGeneration = 0;

    bool behindCompletion = false;
    std::uint32_t labelQueue = 0xffffffffu;
    const auto traceLate = [&](const char* outcome, const std::optional<Graphics::Recorder::LabelHit>& hit, Graphics::Recorder::LabelRefusal refusal) {
        static std::atomic<int> shown{0};
        if (shown.fetch_add(1) >= 200) return;
        std::fprintf(stderr, "[late] queue 0x%x wait on 0x%llx (function %u ref 0x%x mask 0x%x) received %llu: %s; entry value 0x%llx stamp %llu generation %llu, refusal %d\n", queue, static_cast<unsigned long long>(awaited), packet[1] & 7u, packet[4], packet[5], static_cast<unsigned long long>(received), outcome,
                     static_cast<unsigned long long>(hit ? hit->value : 0), static_cast<unsigned long long>(hit ? hit->stamp : 0), static_cast<unsigned long long>(hit ? hit->generation : 0), static_cast<int>(refusal));
    };
    const auto takeLabel = [&](const std::optional<Graphics::Recorder::LabelHit>& hit, Graphics::Recorder::LabelRefusal refusal, bool polling, bool unlockedHit) {
        behindCompletion = refusal == Graphics::Recorder::LabelRefusal::BehindCompletion;
        const bool trace = traceLateLabels() && (refusal != Graphics::Recorder::LabelRefusal::None || (hit.has_value() && hit->late));
        if (!hit.has_value() || !Pm4::WaitComparesValue(packet, hit->value)) {
            if (trace) traceLate(hit.has_value() ? "value mismatch" : "refused by the table", hit, refusal);
            return false;
        }
        if (hit->late) {
            if (hit->generation == refusedGeneration) return false;
            if (!GuestMemory::UnchangedSinceCollected(awaited, awaitedBytes, hit->generation)) {
                ++outcomes.lateRefusedCpuStore;
                refusedGeneration = hit->generation;
                if (trace) traceLate("refused, CPU store since the group closed", hit, refusal);
                return false;
            }
            ++outcomes.fromRecorderLate;
            if (trace) traceLate("trusted", hit, refusal);
        }
        labelQueue = hit->queue;
        ++(polling ? outcomes.fromRecorderPolling : outcomes.fromRecorder);
        if (unlockedHit) ++outcomes.fromRecorderUnlocked;
        // Answered from the label table before the store ran on the GPU: with the async compute lane,
        // a store recorded in the other lane must also precede this queue's later work there.
        if (Graphics::Recorder::LaneOfQueue(hit->queue) != Graphics::Recorder::LaneOfQueue(queue)) Graphics::Recorder::NoteConsumedLabel(awaited, awaitedBytes);
        if (hit->queue == queue) {
            ++outcomes.fromRecorderSameQueue;
            if (!hit->late) epochPoint.bump = false;
        } else {
            notePendingWait(packet, queue, received);
        }
        return true;
    };

    std::uint64_t seenGeneration = Graphics::Recorder::WriteGeneration();

    const bool unlocked = !waitLock && overlapSubmit;
    if (unlocked) {
        if (labelTable) {
            Graphics::Recorder::LabelRefusal refusal{};
            if (takeLabel(Graphics::Recorder::LookupLabel(awaited, awaitedBytes, received, &refusal), refusal, false, true)) return;
        }
    }
    const bool lockedEntry = !unlocked || Graphics::Recorder::SnapshotWriteOverlaps(awaited, awaitedBytes);
    if (!lockedEntry) ++outcomes.entriesUnlocked;

    static const bool blockingEntry = std::getenv("APS5_WAIT_LOCK_ENTRY") != nullptr;
    bool recheck = !lockedEntry;
    bool entryTryFailed = false;
    if (lockedEntry) {
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Wait);
        std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::defer_lock);
        if (blockingEntry || !unlocked) {
            gpuLock.lock();
        } else if (!gpuLock.try_lock()) {
            ++outcomes.entryTriesFailed;
            recheck = true;
            entryTryFailed = true;
        }
        if (const auto localDevice = gpuLock.owns_lock() ? device.Load() : std::shared_ptr<VulkanDevice>{}) {

            if (labelTable) {
                Graphics::Recorder::LabelRefusal refusal{};
                if (takeLabel(localDevice->PendingLabel(awaited, awaitedBytes, received, &refusal), refusal, false, false)) return;
            }

            if (!overlapSubmit || localDevice->OpenWriteOverlaps(awaited, awaitedBytes)) localDevice->SubmitRecorded(false);
        }
    }

    static const bool pollProfile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto& poll = pollStats();
    const auto pollRefused = [&](Graphics::Recorder::LabelRefusal refusal) {
        if (pollProfile && refusal != Graphics::Recorder::LabelRefusal::None && static_cast<std::size_t>(refusal) < poll.refusals.size()) ++poll.refusals[static_cast<std::size_t>(refusal)];
    };

    const bool reapAll = pollReapAll() || !labelShortcut || queue == 0;
    std::chrono::steady_clock::time_point lastTry{};
    const auto pollService = [&](bool sleeping) {
        if (pollProfile) ++poll.ticks;
        const auto generation = Graphics::Recorder::WriteGeneration();
        const bool changed = generation != seenGeneration || recheck;
        const bool labelDue = labelFlushDue();
        const bool reap = sleeping && (behindCompletion || ((reapAll || queue0Dormant.load(std::memory_order_relaxed)) && completionsPending()));
        if (!changed && !labelDue && !reap) return false;

        if (!changed && !pollTryEach()) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastTry < PollTryInterval) return false;
            lastTry = now;
        }
        if (changed && unlocked && labelTable) {

            Graphics::Recorder::LabelRefusal refusal{};
            if (takeLabel(Graphics::Recorder::LookupLabel(awaited, awaitedBytes, received, &refusal), refusal, true, true)) {
                if (pollProfile) ++poll.tableHits;
                return true;
            }
            pollRefused(refusal);
        }
        if (pollProfile) ++poll.tries;
        std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::try_to_lock);
        if (!gpuLock.owns_lock()) {
            ++triesFailed[TryPoll];
            if (pollProfile) ++poll.triesFailed;
            return false;
        }

        struct TryHold {
            bool enabled;
            PollStats& poll;
            std::chrono::steady_clock::time_point start;
            ~TryHold() {
                if (!enabled) return;
                const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
                poll.tryHoldUs += us;
                poll.longestTryHoldUs = std::max(poll.longestTryHoldUs, us);
            }
        } tryHold{pollProfile, poll, pollProfile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
        const auto localDevice = device.Load();
        if (localDevice == nullptr) {
            seenGeneration = generation;
            recheck = false;
            return false;
        }
        bool submitted = false;
        if (changed) {
            seenGeneration = generation;
            recheck = false;
            if (labelTable) {
                Graphics::Recorder::LabelRefusal refusal{};
                if (takeLabel(localDevice->PendingLabel(awaited, awaitedBytes, received, &refusal), refusal, true, false)) {
                    if (pollProfile) ++poll.tableHits;
                    return true;
                }
                pollRefused(refusal);
            }
            if (localDevice->OpenWriteOverlaps(awaited, awaitedBytes)) {
                localDevice->SubmitRecorded(false);
                submitted = true;
            }
        }
        if (labelDue && !submitted && Graphics::Recorder::PendingLabelSince().has_value()) {
            localDevice->SubmitRecorded(false);
            submitted = true;
        }
        if (submitted) {
            ++outcomes.pollSubmits;
            if (pollProfile) ++poll.submits;
        }
        if (reap) {
            const auto withWorkBefore = pollProfile ? Graphics::Recorder::ReapsWithWork() : 0;
            localDevice->ReapRecorded();
            ++outcomes.pollReaps;
            if (pollProfile) {
                ++poll.reaps;
                ++(behindCompletion ? poll.reapsBehindCompletion : poll.reapsOther);
                poll.reapsWithWork += Graphics::Recorder::ReapsWithWork() - withWorkBefore;
            }
        }
        return false;
    };

    if (entryTryFailed && pollService(false)) return;

    noteWaitBlocked(queue, awaited, true);
    struct BlockedWait {
        Driver& driver;
        std::uint32_t queue;
        ~BlockedWait() { driver.noteWaitBlocked(queue, 0, false); }
    } blocked{*this, queue};
    // Diagnostic (APS5_PROFILE_DRAW): blocked waits of 0.5 ms or more by waiting queue, how they ended and which
    // queue produced the value (the label table's or a recorded store's queue, else the newest packet write of the
    // address in the write history), printed every 10 s as [q-wait].
    enum class WaitEnd : std::size_t { Memory, Store, Label, Settled, Count };
    struct Q0WaitNote {
        bool enabled;
        std::uint32_t queue;
        std::chrono::steady_clock::time_point begin;
        WaitEnd end = WaitEnd::Memory;
        std::uint32_t producer = 0xffffffffu;
        std::uint64_t address = 0;
        // [q-split]: the rings' blocked time when this queue-0 wait began.
        std::int64_t beginNs = 0;
        std::array<std::int64_t, 256> cumulativeAtBegin{};
        std::array<std::int64_t, 256> sinceAtBegin{};
        void Begin() {
            if (!enabled || queue >= 256) return;
            beginNs = SteadyNs();
            auto& rings = RingsBlocked();
            rings[queue].sinceNs.store(beginNs, std::memory_order_relaxed);
            GuestMemory::SpecMarkReader(queue, 0);
            GuestMemory::SpecWindowBegin(queue);
            if (queue != 0) return;
            for (std::size_t ring = 0; ring < rings.size(); ++ring) {
                cumulativeAtBegin[ring] = rings[ring].cumulativeNs.load(std::memory_order_relaxed);
                sinceAtBegin[ring] = rings[ring].sinceNs.load(std::memory_order_relaxed);
            }
        }
        void Split(std::int64_t endNs) {
            auto& rings = RingsBlocked();
            if (queue < 256) {
                rings[queue].cumulativeNs.fetch_add(endNs - beginNs, std::memory_order_relaxed);
                rings[queue].sinceNs.store(0, std::memory_order_relaxed);
                rings[queue].producer.store(producer, std::memory_order_relaxed);
            }
            GuestMemory::SpecWindowEnd(queue, (end == WaitEnd::Label || end == WaitEnd::Store) && producer < 256 && producer != queue, producer, endNs - beginNs);
            AgcDriver::TraceDepTag = (1ull << 60u) | (static_cast<std::uint64_t>(static_cast<std::size_t>(end) + 1u) << 52u) | (producer & 0xffffu);
            if (queue != 0 || (end != WaitEnd::Label && end != WaitEnd::Store) || producer >= 256 || producer == 0) return;
            const auto total = endNs - beginNs;
            auto blocked = rings[producer].cumulativeNs.load(std::memory_order_relaxed) - cumulativeAtBegin[producer];
            if (sinceAtBegin[producer] != 0) blocked -= beginNs - sinceAtBegin[producer];
            if (const auto since = rings[producer].sinceNs.load(std::memory_order_relaxed); since != 0) blocked += endNs - std::max(since, beginNs);
            blocked = std::clamp<std::int64_t>(blocked, 0, total);
            struct Ring { std::uint64_t waits = 0; std::int64_t totalNs = 0, blockedNs = 0; std::map<std::uint32_t, std::uint64_t> on; };
            static std::mutex mutex;
            static std::map<std::uint32_t, Ring> split;
            static auto last = std::chrono::steady_clock::now();
            std::lock_guard lock(mutex);
            auto& ring = split[producer];
            ++ring.waits;
            ring.totalNs += total;
            ring.blockedNs += blocked;
            ++ring.on[rings[producer].producer.load(std::memory_order_relaxed)];
            const auto now = std::chrono::steady_clock::now();
            if (now - last < std::chrono::seconds(10)) return;
            last = now;
            const auto labelNs = DrawPipeline::LabelCommitNs().exchange(0, std::memory_order_relaxed);
            const auto labels = DrawPipeline::LabelCommits().exchange(0, std::memory_order_relaxed);
            char head[160];
            std::snprintf(head, sizeof(head), "[q-split] (10 s) queue 0's waits ended by ring R's label or store; queue 0 label items enqueue->commit %llu x %.0f us:", static_cast<unsigned long long>(labels), labels != 0 ? static_cast<double>(labelNs) / 1e3 / static_cast<double>(labels) : 0.0);
            std::string line = head;
            for (const auto& [from, value] : split) {
                char item[200];
                std::snprintf(item, sizeof(item), " R=0x%x %llu waits %.0f ms = R blocked %.0f + R working %.0f (R's last wait on:", from, static_cast<unsigned long long>(value.waits), static_cast<double>(value.totalNs) / 1e6, static_cast<double>(value.blockedNs) / 1e6, static_cast<double>(value.totalNs - value.blockedNs) / 1e6);
                line += item;
                for (const auto& [on, count] : value.on) {
                    std::snprintf(item, sizeof(item), " 0x%x x%llu", on == 0xffffffffu ? 0xffu : on, static_cast<unsigned long long>(count));
                    line += item;
                }
                line += ");";
            }
            AgcDriver::ProfilePrint_nid_no_patch("%s\n", line.c_str());
            split.clear();
        }
        ~Q0WaitNote() {
            if (CeilingNoCrossWait() && (end == WaitEnd::Label || end == WaitEnd::Store) && producer < 256 && producer != queue) {
                std::lock_guard lock(CrossWaitMutex());
                CrossWaitAddresses().insert(address);
            }
            if (!enabled) return;
            Split(SteadyNs());
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            if (ms < 0.5) return;
            struct Bucket { std::uint64_t count = 0; double ms = 0.0, maxMs = 0.0; };
            static std::mutex mutex;
            static std::map<std::tuple<std::uint32_t, std::size_t, std::uint32_t>, Bucket> buckets;
            static auto last = std::chrono::steady_clock::now();
            std::lock_guard lock(mutex);
            auto& bucket = buckets[{queue, static_cast<std::size_t>(end), producer}];
            ++bucket.count;
            bucket.ms += ms;
            bucket.maxMs = std::max(bucket.maxMs, ms);
            const auto now = std::chrono::steady_clock::now();
            if (now - last < std::chrono::seconds(10)) return;
            last = now;
            static constexpr const char* names[] = {"memory", "store", "label", "settled"};
            std::string line = "[q-wait] (10 s) blocked waits >= 0.5 ms, waiting queue <- end/producer queue:";
            for (const auto& [key, value] : buckets) {
                const auto& [waiter, how, from] = key;
                char item[128];
                std::snprintf(item, sizeof(item), " 0x%x<-%s/0x%x %llu %.0fms max %.1f", waiter, names[how], from == 0xffffffffu ? 0xffu : from, static_cast<unsigned long long>(value.count), value.ms, value.maxMs);
                line += item;
            }
            AgcDriver::ProfilePrint_nid_no_patch("%s\n", line.c_str());
            buckets.clear();
        }
    };
    if (CeilingNoCrossWait()) {
        bool learned = false;
        {
            std::lock_guard lock(CrossWaitMutex());
            learned = CrossWaitAddresses().contains(awaited);
        }
        if (learned) {
            const auto skipped = crossWaitsSkipped.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((skipped & 0x3fffu) == 0) std::fprintf(stderr, "[ceiling] %llu cross-ring waits skipped\n", static_cast<unsigned long long>(skipped));
            return;
        }
    }
    static const bool q0WaitProfile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    Q0WaitNote q0Wait{q0WaitProfile, queue, std::chrono::steady_clock::now()};
    q0Wait.address = awaited;
    q0Wait.Begin();
    static const bool pauseSpin = std::getenv("APS5_NO_PAUSE_SPIN") == nullptr;
    static const bool storeHeldResults = std::getenv("APS5_NO_QUEUED_DRAW_READ_ORDER") == nullptr;
    const auto spinLimit = queue == 0 ? std::chrono::microseconds(1500) : std::chrono::microseconds(100);
    const auto spinStart = std::chrono::steady_clock::now();
    bool spinning = pauseSpin;
    std::uint32_t polls = 0;
    StallWatch stall(queue);
    while (!Pm4::WaitSatisfiedUnchecked(packet)) {
        if (std::uint32_t writer = queue; !requireMemory && storedSince(packet, awaited, awaitedBytes, received, &writer)) {
            q0Wait.end = WaitEnd::Store;
            q0Wait.producer = writer;
            if (writer != queue) notePendingWait(packet, queue, received);
            else epochPoint.bump = false;
            return;
        }
        ++polls;
        if (spinning) {
            _mm_pause();
            if ((polls & 63u) != 0) continue;
            if (std::chrono::steady_clock::now() - spinStart > spinLimit) spinning = false;
        } else if (pauseSpin || polls >= 4000) {
            PollSleep();
        } else {
            std::this_thread::yield();
        }
        CheckFailure();
        if (pollService(!spinning)) {
            q0Wait.end = WaitEnd::Label;
            q0Wait.producer = labelQueue;
            return;
        }
        if (storeHeldResults && !spinning && (polls & 31u) == 0) GuestMemory::FlushGpuWrites(awaited, awaitedBytes);
        noteAwaitingTitle(queue, awaited);
        if (requireMemory && (polls & 15u) == 0) {
            std::unique_lock settleLock(GuestMemory::GpuMutex(), std::try_to_lock);
            if (settleLock.owns_lock()) {
                if (const auto localDevice = device.Load(); localDevice != nullptr && localDevice->RecordedWritesSettled(awaited, awaitedBytes)) {
                    q0Wait.end = WaitEnd::Settled;
                    return;
                }
            }
        }
        if (const auto done = packetsDone.load(); done != lastDone || (!requireMemory && packetsInFlight.load() != 0)) {
            lastDone = done;
            start = std::chrono::steady_clock::now();
            stall.Progressed();
        }
        stall.Check(((packet[0] >> 8u) & 0xffu) == 0x93u ? "WAIT_REG_MEM_64 at" : "WAIT_REG_MEM at", awaited);

        if (!warned && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(waitTimeoutMs())) {
            warned = true;
            ++outcomes.timedOut;
            {
                static std::mutex reportedMutex;
                static std::set<std::uint64_t> reported;
                static std::uint64_t timeouts = 0;
                const std::lock_guard lock(reportedMutex);
                if (++timeouts % 20 == 0) std::fprintf(stderr, "[gpu] %llu GPU waits have timed out\n", static_cast<unsigned long long>(timeouts));
                if (!reported.insert(awaited).second) continue;
            }
            const bool wide = ((packet[0] >> 8u) & 0xffu) == 0x93u;
            const std::uint64_t reference = wide ? packet[4] | (static_cast<std::uint64_t>(packet[5]) << 32u) : packet[4];
            const std::uint64_t mask = wide ? packet[6] | (static_cast<std::uint64_t>(packet[7]) << 32u) : packet[5];
            const std::uint64_t current = awaitedBytes == 8 ? *reinterpret_cast<const volatile std::uint64_t*>(awaited) : *reinterpret_cast<const volatile std::uint32_t*>(awaited);
            std::fprintf(stderr, "[gpu] queue 0x%x WAIT_REG_MEM%s%s at 0x%llx timed out after %dms (function %u ref 0x%llx mask 0x%llx value 0x%llx)\n", queue, wide ? "_64" : "", requireMemory ? " (landing)" : "",
                         static_cast<unsigned long long>(awaited), waitTimeoutMs(), packet[1] & 7u, static_cast<unsigned long long>(reference), static_cast<unsigned long long>(mask), static_cast<unsigned long long>(current));
            const auto address = awaited;
            for (const auto& record : writeHistory()) {
                if (record.length != 0 && record.target <= address && address < record.target + std::max<std::uint64_t>(record.length, 4))
                    std::fprintf(stderr, "[gpu]   earlier write by queue 0x%x opcode 0x%x at 0x%llx+0x%llx\n", record.queue, record.opcode, static_cast<unsigned long long>(record.target), static_cast<unsigned long long>(record.length));
            }

            if (const auto reportDevice = device.Load(); reportDevice != nullptr) {
                Graphics::Recorder::LabelRefusal refusal{};
                const auto hit = reportDevice->PendingLabel(awaited, awaitedBytes, received, &refusal);
                const auto since = Graphics::Recorder::PendingLabelSince();
                const auto age = since.has_value() ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - *since).count() : -1.0;
                std::fprintf(stderr, "[gpu]   recorder: table %s (refusal %u), open batch writes it %d, snapshot write overlaps %d, pending completion labels %llu, write-back completions %llu, pending label age %.1f ms, write generation %llu seen %llu, queue 0 dormant %d, completions pending %d, label due %d\n",
                             hit.has_value() ? "hit" : "miss", static_cast<unsigned>(refusal), reportDevice->OpenWriteOverlaps(awaited, awaitedBytes) ? 1 : 0, Graphics::Recorder::SnapshotWriteOverlaps(awaited, awaitedBytes) ? 1 : 0,
                             static_cast<unsigned long long>(Graphics::Recorder::PendingCompletionLabels()), static_cast<unsigned long long>(Graphics::Recorder::PendingWriteBackCompletions()), age,
                             static_cast<unsigned long long>(Graphics::Recorder::WriteGeneration()), static_cast<unsigned long long>(seenGeneration), queue0Dormant.load(std::memory_order_relaxed) ? 1 : 0, completionsPending() ? 1 : 0, labelFlushDue() ? 1 : 0);
            }
            context.Print(stderr, "[gpu]   preceding packet %s\n");
        }
    }
    if (q0Wait.enabled) {
        const auto& history = writeHistory();
        const auto cursor = writeCursor();
        for (std::size_t back = 1; back <= history.size(); ++back) {
            const auto& record = history[(cursor - back) % history.size()];
            if (record.length != 0 && record.target <= awaited && awaited < record.target + std::max<std::uint64_t>(record.length, 4)) {
                q0Wait.producer = record.queue;
                break;
            }
        }
    }
    ++outcomes.polled;
}

}
