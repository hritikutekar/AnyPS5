#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_WORKERAFFINITY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_WORKERAFFINITY_HPP

#include <cstdint>

namespace AgcDriver::DriverDetail {

std::uint64_t WorkerAffinityMask();

void PinWorkerThread(const char* role);

// Names the calling thread for the OS (Linux: /proc/<pid>/task/<tid>/comm, shown by top -H and perf;
// at most 15 characters, "queue worker 0x<id>" becomes "agc q0x<id>"). Windows names come from
// PinWorkerThread's SetThreadDescription.
void NameCurrentThread(const char* role);

// Thread pinning (Linux, non-hybrid CPUs with at least 6 cores; APS5_NO_THREAD_PINNING=1 turns it off, an
// APS5_AB switch): the queue-0 worker, the draw committer and the draw preparer each run alone on one
// logical CPU of its own physical core (the cores at 1/4, 1/2 and 3/4 of the core list; their SMT siblings
// stay idle), and every other thread of the process on the remaining cores. They hand work to each other
// every draw, so sharing a core with each other or with game threads, or migrating, slows the frame
// (best261: +3.3 % world presents with the same placement applied from outside).
enum class CriticalThread { Queue0Worker, Committer, Preparer };

// Records the calling thread as `role`; the placement itself is applied by FollowThreadPinning.
void NoteCriticalThread(CriticalThread role);

// The queue-0 worker at a present: applies a change of the switch at once, and otherwise places every
// thread again at most once a second (threads started since inherit their creator's CPUs, possibly a
// critical thread's single CPU).
void FollowThreadPinning();

}

#endif
