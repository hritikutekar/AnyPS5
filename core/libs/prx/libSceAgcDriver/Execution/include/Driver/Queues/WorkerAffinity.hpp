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

}

#endif
