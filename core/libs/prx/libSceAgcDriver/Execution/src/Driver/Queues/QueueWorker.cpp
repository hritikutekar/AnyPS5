#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/SubmissionRing.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/WaitMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/WorkerSampler.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <algorithm>
#include <chrono>
#include <mutex>
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <cstdlib>

namespace AgcDriver::DriverDetail {

namespace {

constexpr auto IdleResultsQuietPeriod = std::chrono::milliseconds(50);

}

FrameTiming* Driver::frameTiming() {
    if (!APS5_ENABLE_TIMING_LOG) return nullptr;
    if (pendingFrameTiming == nullptr) pendingFrameTiming = std::make_shared<FrameTiming>(++frameSerial, true);
    return pendingFrameTiming.get();
}

FrameTiming* Driver::includeTimingSubmission(const Submission& submission, bool firstSegment) {
    if (!APS5_ENABLE_TIMING_LOG) return nullptr;
    auto* frame = submission.queue == 0 ? frameTiming() : FrameTiming::Async();
    require(submission.receivedAt != FrameTiming::Clock::time_point{} && submission.receivedAt <= submission.enqueuedAt && submission.enqueuedAt <= submission.dequeuedAt && submission.dequeuedAt <= submission.orderedAt, "invalid submission timing order");
    if (submission.queue == 0) frame->IncludeSubmission(submission.serial, submission.receivedAt, submission.enqueuedAt, submission.orderedAt, firstSegment);
    else if (firstSegment) {
        frame->Add(frame->Get("Submission", "accept"), submission.enqueuedAt - submission.receivedAt);
        frame->Add(frame->Get("Submission", "queue"), submission.orderedAt - submission.enqueuedAt);
    }
    if (firstSegment) {
        frame->Add(frame->Get("Submission", "dequeue"), submission.dequeuedAt - submission.enqueuedAt);
        frame->Add(frame->Get("Submission", "order_wait"), submission.orderedAt - submission.dequeuedAt);
        if (!submission.suspend) {
            frame->Add(frame->Get("Submission", "copy"), submission.copiedAt - submission.receivedAt, submission.commands.size() * sizeof(std::uint32_t));
            frame->Add(frame->Get("Submission", "validate"), submission.validatedAt - submission.copiedAt);
            frame->Add(frame->Get("Submission", "flip_room_wait"), submission.roomReadyAt - submission.validatedAt);
            frame->Add(frame->Get("Submission", "reserve_enqueue"), submission.enqueuedAt - submission.roomReadyAt);
        }
    }
    return frame;
}

void Driver::markCompleted(std::uint64_t serial) {
    completedOutOfOrder.insert(serial);
    while (!completedOutOfOrder.empty() && *completedOutOfOrder.begin() == completed + 1) {
        completed = *completedOutOfOrder.begin();
        completedOutOfOrder.erase(completedOutOfOrder.begin());
    }
}

bool& Driver::completionDeferred() {
    static thread_local bool deferred = false;
    return deferred;
}

void Driver::deferCompletion(const Submission& submission) {
    std::uint64_t received = submission.received;
    {
        std::lock_guard lock(mutex);
        if (queue0Executing != 0) received = std::min(received, queue0Executing);
        queue0Uncommitted.push_back(received);
    }
    completionDeferred() = true;
    DrawPipeline::Queue0().Enqueue([this, serial = submission.serial, received, labelWrites = submission.labelWrites] {
        completeSubmission(serial, received, labelWrites);
    }, {});
}

void Driver::submitOpenWork() {
    if (deferredLabels().labels.empty() && !Graphics::Recorder::PendingLabelSince().has_value() && Graphics::Recorder::RecordedWorkSinceSubmit() == 0) return;
    ++submissionCosts(0).endSubmits;
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::End);
    std::lock_guard gpuLock(GuestMemory::GpuMutex());
    const auto localDevice = device.Load();
    recordDeferredLabels(localDevice.get(), 0);
    if (localDevice != nullptr) localDevice->SubmitRecorded(true);
}

void Driver::wakeQueueZeroForCompletions() {
    if (!completionsPending()) return;
    std::lock_guard lock(mutex);
    changed.notify_all();
}

void Driver::completeSubmission(std::uint64_t serial, std::uint64_t received, std::span<const std::uint64_t> labelWrites) {
    struct Complete {
        Driver& driver;
        std::uint64_t serial, received;
        std::span<const std::uint64_t> labelWrites;
        ~Complete() {
            bool notify = true;
            {
                std::lock_guard lock(driver.mutex);
                driver.markCompleted(serial);
                driver.forgetUnfinishedWrites(driver.workers.at(0), labelWrites);
                const auto found = std::find(driver.queue0Uncommitted.begin(), driver.queue0Uncommitted.end(), received);
                if (found != driver.queue0Uncommitted.end()) driver.queue0Uncommitted.erase(found);
                notify = driver.idleWaiters != 0 || driver.orderHolders.load(std::memory_order_acquire) != 0 || completionsPending();
            }
            if (notify) driver.changed.notify_all();
        }
    } complete{*this, serial, received, labelWrites};
    submitOpenWork();
}

const std::atomic<std::uint64_t>*& Driver::workerQueued() {
    static thread_local const std::atomic<std::uint64_t>* queued = nullptr;
    return queued;
}

void Driver::reapCompletionLabels() {
    std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::try_to_lock);
    if (!gpuLock.owns_lock()) {
        ++triesFailed[TryIdle];
        return;
    }

    if (!completionsPending()) return;

    bumpEpoch(&EpochBumps::reaps);
    if (const auto localDevice = device.Load()) localDevice->ReapRecorded();
}

namespace {

// Diagnostic (APS5_PROFILE_DRAW): for queue 0's submissions, whether each was enqueued before the previous
// execute ended (and how long before) or after it (the worker waited with nothing queued); [q0-dequeue].
struct Queue0Dequeues {
    std::mutex mutex;
    std::chrono::steady_clock::time_point lastEnd{}, lastEop{}, report = std::chrono::steady_clock::now();
    std::uint64_t before = 0, after = 0, suspends = 0, eops = 0, afterEop = 0;
    double beforeMs = 0, afterMs = 0, endToEopMs = 0, eopToEnqueueMs = 0;
};

Queue0Dequeues& queue0Dequeues() {
    static Queue0Dequeues stats;
    return stats;
}

bool queue0DequeuesProfiled() {
    static const bool profiled = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profiled;
}

}

// The flip's EOP interrupt of queue 0 delivered to the title (after the recorded work completed on the GPU).
void NoteQueue0FlipEop() {
    if (!queue0DequeuesProfiled()) return;
    auto& stats = queue0Dequeues();
    std::lock_guard lock(stats.mutex);
    const auto now = std::chrono::steady_clock::now();
    stats.lastEop = now;
    ++stats.eops;
    if (stats.lastEnd != std::chrono::steady_clock::time_point{}) stats.endToEopMs += std::chrono::duration<double, std::milli>(now - stats.lastEnd).count();
}

namespace {

void noteQueue0ExecuteEnd() {
    if (!queue0DequeuesProfiled()) return;
    auto& stats = queue0Dequeues();
    std::lock_guard lock(stats.mutex);
    stats.lastEnd = std::chrono::steady_clock::now();
}

void noteQueue0Dequeue(std::chrono::steady_clock::time_point enqueuedAt, bool suspend) {
    if (!queue0DequeuesProfiled()) return;
    auto& stats = queue0Dequeues();
    std::lock_guard lock(stats.mutex);
    if (suspend) ++stats.suspends;
    if (stats.lastEnd != std::chrono::steady_clock::time_point{} && enqueuedAt != std::chrono::steady_clock::time_point{}) {
        const auto ms = std::chrono::duration<double, std::milli>(enqueuedAt - stats.lastEnd).count();
        if (ms <= 0) { ++stats.before; stats.beforeMs -= ms; } else { ++stats.after; stats.afterMs += ms; }
        if (!suspend && stats.lastEop != std::chrono::steady_clock::time_point{}) {
            ++stats.afterEop;
            stats.eopToEnqueueMs += std::chrono::duration<double, std::milli>(enqueuedAt - stats.lastEop).count();
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - stats.report < std::chrono::seconds(10)) return;
    AgcDriver::ProfilePrint_nid_no_patch("[q0-dequeue] (10 s) submissions enqueued before the previous execute ended: %llu (avg %.1f ms before), after it: %llu (avg %.1f ms after, the worker waited with nothing queued); %llu suspend boundaries; flip EOPs %llu, avg %.1f ms after the execute ended; submissions enqueued avg %.1f ms after the latest flip EOP (%llu)\n",
        static_cast<unsigned long long>(stats.before), stats.before ? stats.beforeMs / static_cast<double>(stats.before) : 0.0, static_cast<unsigned long long>(stats.after), stats.after ? stats.afterMs / static_cast<double>(stats.after) : 0.0, static_cast<unsigned long long>(stats.suspends),
        static_cast<unsigned long long>(stats.eops), stats.eops ? stats.endToEopMs / static_cast<double>(stats.eops) : 0.0, stats.afterEop ? stats.eopToEnqueueMs / static_cast<double>(stats.afterEop) : 0.0, static_cast<unsigned long long>(stats.afterEop));
    stats.before = stats.after = stats.suspends = stats.eops = stats.afterEop = 0;
    stats.beforeMs = stats.afterMs = stats.endToEopMs = stats.eopToEnqueueMs = 0;
    stats.report = now;
}

}

void Driver::run(std::uint32_t id) noexcept {
    onWorkerThread() = true;
    char role[32];
    std::snprintf(role, sizeof role, "queue worker 0x%x", id);
    PinWorkerThread(role);
    if (id == 0) NoteCriticalThread(CriticalThread::Queue0Worker);

    GuestMemory::TagGpuLockThread(id);
    if (id == 0) StartWorkerSampler();
    Submission submission;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto& costs = submissionCosts(id);
    try {
        if (PerformanceTrace::Configured()) PerformanceTrace::BindThread(role);
        for (;;) {
            submission = Submission{};
            struct Running {
                std::atomic<std::uint32_t>& count;
                std::atomic<std::uint64_t>& idleEpoch;
                std::condition_variable& changed;
                bool active = false;
                Running(std::atomic<std::uint32_t>& count, std::atomic<std::uint64_t>& idleEpoch, std::condition_variable& changed)
                    : count(count), idleEpoch(idleEpoch), changed(changed) {}
                void Start() {
                    count.fetch_add(1, std::memory_order_acq_rel);
                    active = true;
                }
                ~Running() {
                    if (active && count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        idleEpoch.fetch_add(1, std::memory_order_release);
                        changed.notify_all();
                    }
                }
            } running{runningWorkers, idleEpoch, changed};
            static const bool traceGpu = std::getenv("APS5_TRACE_GPU") != nullptr;
            {
                PerformanceContext timingContext(id == 0 ? frameTiming() : FrameTiming::Async());
                PerformanceTimer timing("Driver.WorkerWait");
                std::unique_lock lock(mutex);
                timing.Mark("mutex_wait");
                auto& worker = workers.at(id);
                auto& pending = worker.pending;
                workerQueued() = &worker.queued;
                if (traceGpu && pending.empty()) std::fprintf(stderr, "[gpu] %.1f idle queue=0x%x\n", TraceMs(), id);
                const auto ready = [&] { return failure || stopping || !pending.empty(); };
                const auto allQueuesIdle = [&] {
                    return runningWorkers.load(std::memory_order_acquire) == 0
                        && std::all_of(workers.begin(), workers.end(), [](const auto& item) { return item.second.pending.empty(); });
                };
                const auto idleResultsPending = [&] {
                    return id == 0 && Graphics::StorageTexture::AnyPending() && allQueuesIdle() && !completionsPending();
                };
                const auto poll = [&](const auto& done) {
#ifdef _WIN32
                    lock.unlock();
                    PollSleep();
                    lock.lock();
                    return done();
#else
                    return changed.wait_for(lock, std::chrono::milliseconds(1), done);
#endif
                };

                while (!ready()) {
                    if (!completionsPending() || (id != 0 && Graphics::Recorder::PendingCompletionLabels() == 0)) {
                        const auto idleEpochAtCheck = idleEpoch.load(std::memory_order_acquire);
                        const bool storeIdleResults = idleResultsPending();
                        if (storeIdleResults) {
                            queue0Dormant.store(true, std::memory_order_relaxed);
                            const auto quietUntil = std::chrono::steady_clock::now() + IdleResultsQuietPeriod;
                            const auto idleEpochAtWait = idleEpoch.load(std::memory_order_acquire);
                            changed.wait_until(lock, quietUntil, [&] {
                                return ready() || idleEpoch.load(std::memory_order_acquire) != idleEpochAtWait;
                            });
                            queue0Dormant.store(false, std::memory_order_relaxed);
                            if (ready()) break;
                            if (idleEpoch.load(std::memory_order_acquire) != idleEpochAtWait) {
                                continue;
                            }
                            if (!idleResultsPending()) continue;
                        }
                        lock.unlock();
                        if (storeIdleResults) {
                            if (const auto localDevice = device.Load()) localDevice->StoreIdleResults();
                        }
                        lock.lock();
                        if (ready()) break;
                        if (id == 0 && allQueuesIdle() && !completionsPending()
                            && ((!storeIdleResults && Graphics::StorageTexture::AnyPending()) || idleEpoch.load(std::memory_order_acquire) != idleEpochAtCheck)) continue;
                        if (id == 0) queue0Dormant.store(true, std::memory_order_relaxed);
                        const auto idleEpochAtWait = idleEpoch.load(std::memory_order_acquire);
                        changed.wait(lock, [&] { return ready() || (id == 0 && (idleEpoch.load(std::memory_order_acquire) != idleEpochAtWait || completionsPending())); });
                        if (id == 0) queue0Dormant.store(false, std::memory_order_relaxed);
                        if (id == 0 && !ready() && (idleEpoch.load(std::memory_order_acquire) != idleEpochAtWait || completionsPending())) continue;
                        break;
                    }
                    if (poll(ready)) break;
                    lock.unlock();
                    reapCompletionLabels();
                    lock.lock();
                }
                timing.Mark("idle_and_completion_poll");
                rethrowFailure();
                if (stopping || shutdownToken.stop_requested() || pending.empty()) {
                    break;
                }
                submission = std::move(pending.front());
                pending.pop_front();
                if (id == 0) noteQueue0Dequeue(submission.enqueuedAt, submission.suspend);
                if (APS5_ENABLE_TIMING_LOG) submission.dequeuedAt = std::chrono::steady_clock::now();
                if (id == 0) queue0Executing = submission.suspend ? 0 : submission.received;
                if (submission.waitFree) {
                    {
                        struct OrderHold {
                            std::atomic<std::uint32_t>& holders;
                            explicit OrderHold(std::atomic<std::uint32_t>& holders) : holders(holders) { holders.fetch_add(1, std::memory_order_acq_rel); }
                            ~OrderHold() { holders.fetch_sub(1, std::memory_order_acq_rel); }
                            OrderHold(const OrderHold&) = delete;
                            OrderHold& operator=(const OrderHold&) = delete;
                        } hold{orderHolders};
                        const auto released = [&] { return failure || stopping || orderReleased(id, submission.received); };
                        StallWatch stall(id);
                        while (!released() && !poll(released)) stall.Check("order behind queue 0 for event", submission.received);
                    }
                    rethrowFailure();
                }
                if (APS5_ENABLE_TIMING_LOG) submission.orderedAt = std::chrono::steady_clock::now();
                timing.Mark("dequeue_order");
                worker.queued.fetch_sub(1, std::memory_order_acq_rel);
                running.Start();
                if (profile && submission.enqueuedAt != std::chrono::steady_clock::time_point{}) costs.dequeueNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - submission.enqueuedAt).count());
            }
            execute(submission);
            if (id == 0) noteQueue0ExecuteEnd();
            if (id == 0) DrawPipeline::WorkerActivity().store(static_cast<std::uint8_t>(DrawPipeline::WorkerState::BetweenSubmissions), std::memory_order_relaxed);
            if (traceGpu) std::fprintf(stderr, "[gpu] %.1f done serial=%llu queue=0x%x\n", TraceMs(), static_cast<unsigned long long>(submission.serial), id);
            const auto completeStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            PerformanceContext completionContext(id == 0 ? frameTiming() : FrameTiming::Async());
            PerformanceTimer completionTiming("Driver.Completion");
            bool notify = true;
            {
                std::lock_guard lock(mutex);
                rethrowFailure();
                if (!std::exchange(completionDeferred(), false)) {
                    markCompleted(submission.serial);
                    forgetUnfinishedWrites(workers.at(id), submission);
                }
                if (id == 0) queue0Executing = 0;

                notify = idleWaiters != 0 || orderHolders.load(std::memory_order_acquire) != 0;
            }
            if (notify) changed.notify_all();
            else ++costs.notifiesSkipped;
            if (profile) costs.completeNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - completeStart).count());
        }
    } catch (const ProcessShutdown&) {
        submission = Submission{};
    } catch (...) {
        const auto error = std::current_exception();
        for (const auto& [offset, flip] : submission.flips) flip->Fail(error);
        ReportFailure(error);
    }
}

}
