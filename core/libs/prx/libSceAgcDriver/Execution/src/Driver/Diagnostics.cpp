#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include <algorithm>
#include <cstdlib>
#include <vector>
#include <stdexcept>

namespace AgcDriver::DriverDetail {

double TraceMs() {
    static const auto origin = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - origin).count();
}

// APS5_SKIP_SHADER=<hex address>[,...]: programs whose dispatches and draws are skipped.
const std::vector<std::uint64_t>& SkippedShaders() {
    static const std::vector<std::uint64_t> skipped = [] {
        std::vector<std::uint64_t> addresses;
        if (const char* text = std::getenv("APS5_SKIP_SHADER")) {
            for (const char* at = text; *at != '\0';) {
                char* end = nullptr;
                const auto address = std::strtoull(at, &end, 16);
                if (end == at) break;
                if (address != 0) addresses.push_back(address);
                at = *end == ',' ? end + 1 : end;
            }
        }
        return addresses;
    }();
    return skipped;
}

std::uint64_t SkippedShader() {
    return SkippedShaders().empty() ? 0 : SkippedShaders().front();
}

bool IsSkippedShader(std::uint64_t address) {
    const auto& skipped = SkippedShaders();
    return address != 0 && std::find(skipped.begin(), skipped.end(), address) != skipped.end();
}

void require(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(std::string("AGC driver: ") + reason);
    }
}

std::uint32_t readRegister(const Registers& registers, std::uint32_t offset) {
    const auto it = registers.find(offset);
    require(it != registers.end(), "required shader register has not been written");
    return it->second;
}

std::uint32_t readUserData(const Registers& shader, std::uint32_t offset) {
    const auto it = shader.find(offset);
    return it == shader.end() ? 0u : it->second;
}

}
