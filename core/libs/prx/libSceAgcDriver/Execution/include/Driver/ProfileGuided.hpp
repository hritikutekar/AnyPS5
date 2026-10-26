#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_PROFILEGUIDED_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_PROFILEGUIDED_HPP

#include <cstdint>

namespace AgcDriver::DriverDetail {

// A present of queue 0. In a build instrumented for profile-guided optimization (ANYPS5_AGC_PGO=generate)
// it writes the profile once, at APS5_PGO_DUMP_AT (default 2600); otherwise it returns at once. Out of line
// and the same in every build, so the instrumented and the optimized build see the same control flow.
void NotePgoPresent(std::uint64_t present);

}

#endif
