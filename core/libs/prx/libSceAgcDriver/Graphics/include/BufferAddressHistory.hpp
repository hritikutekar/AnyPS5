#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BUFFERADDRESSHISTORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BUFFERADDRESSHISTORY_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Debug aid: APS5_BUFFER_VA_HISTORY=1 wraps vkCreateBuffer, vkGetBufferDeviceAddress(KHR) and
// vkDestroyBuffer as Context::Function resolves them, and keeps every buffer's device address range
// with its creating call site, live or destroyed (the most recent destroyed ones). The device fault
// report (DeviceFault.hpp) then names the buffer that owned a faulting address and whether it was
// already destroyed. Call sites print as <module>+<offset> (resolve with addr2line -f -C -e <module>).
namespace AgcDriver::Graphics::BufferAddressHistory {

struct Entry {
    VkDeviceSize bytes = 0;
    VkDeviceAddress address = 0;
    void* site = nullptr;
    double createdMs = 0;
    double destroyedMs = -1;
};

struct State {
    std::mutex mutex;
    PFN_vkCreateBuffer create = nullptr;
    PFN_vkDestroyBuffer destroy = nullptr;
    PFN_vkGetBufferDeviceAddress address = nullptr;
    PFN_vkGetBufferDeviceAddress addressKhr = nullptr;
    std::unordered_map<VkBuffer, Entry> live;
    std::deque<Entry> destroyed;
    std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
};

inline bool Enabled() {
    static const bool enabled = std::getenv("APS5_BUFFER_VA_HISTORY") != nullptr;
    return enabled;
}

inline State& Get() {
    static State state;
    return state;
}

inline double NowMs(const State& state) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - state.origin).count();
}

inline std::string Site(void* site) {
    Dl_info info{};
    if (site == nullptr || dladdr(site, &info) == 0 || info.dli_fname == nullptr) return "?";
    const char* module = std::strrchr(info.dli_fname, '/') != nullptr ? std::strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
    char text[160];
    std::snprintf(text, sizeof(text), "%s+0x%llx", module, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(site) - reinterpret_cast<std::uintptr_t>(info.dli_fbase)));
    return text;
}

inline VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice device, const VkBufferCreateInfo* info, const VkAllocationCallbacks* callbacks, VkBuffer* buffer) {
    auto& state = Get();
    void* const site = __builtin_return_address(0);
    const auto result = state.create(device, info, callbacks, buffer);
    if (result != VK_SUCCESS) return result;
    std::lock_guard lock(state.mutex);
    state.live[*buffer] = Entry{info->size, 0, site, NowMs(state), -1};
    return result;
}

inline VkDeviceAddress Note(VkBuffer buffer, VkDeviceAddress address) {
    auto& state = Get();
    std::lock_guard lock(state.mutex);
    if (auto it = state.live.find(buffer); it != state.live.end()) it->second.address = address;
    return address;
}

inline VKAPI_ATTR VkDeviceAddress VKAPI_CALL GetAddress(VkDevice device, const VkBufferDeviceAddressInfo* info) {
    return Note(info->buffer, Get().address(device, info));
}

inline VKAPI_ATTR VkDeviceAddress VKAPI_CALL GetAddressKhr(VkDevice device, const VkBufferDeviceAddressInfo* info) {
    return Note(info->buffer, Get().addressKhr(device, info));
}

inline VKAPI_ATTR void VKAPI_CALL DestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* callbacks) {
    auto& state = Get();
    if (buffer != VK_NULL_HANDLE) {
        std::lock_guard lock(state.mutex);
        if (auto it = state.live.find(buffer); it != state.live.end()) {
            if (it->second.address != 0) {
                it->second.destroyedMs = NowMs(state);
                state.destroyed.push_back(it->second);
                if (state.destroyed.size() > 400000) state.destroyed.pop_front();
            }
            state.live.erase(it);
        }
    }
    state.destroy(device, buffer, callbacks);
}

inline PFN_vkVoidFunction Hook(const char* name, PFN_vkVoidFunction real) {
    if (!Enabled() || real == nullptr || std::strncmp(name, "vk", 2) != 0) return real;
    auto& state = Get();
    std::lock_guard lock(state.mutex);
    if (std::strcmp(name, "vkCreateBuffer") == 0) {
        if (state.create == nullptr) state.create = reinterpret_cast<PFN_vkCreateBuffer>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(&CreateBuffer);
    }
    if (std::strcmp(name, "vkDestroyBuffer") == 0) {
        if (state.destroy == nullptr) state.destroy = reinterpret_cast<PFN_vkDestroyBuffer>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(&DestroyBuffer);
    }
    if (std::strcmp(name, "vkGetBufferDeviceAddress") == 0) {
        if (state.address == nullptr) state.address = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(&GetAddress);
    }
    if (std::strcmp(name, "vkGetBufferDeviceAddressKHR") == 0) {
        if (state.addressKhr == nullptr) state.addressKhr = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(&GetAddressKhr);
    }
    return real;
}

// The buffers whose device range holds `address` (live ones, then the latest destroyed ones), and
// the nearest live ranges below and above it when none does.
inline std::vector<std::string> Describe(VkDeviceAddress address) {
    std::vector<std::string> lines;
    if (!Enabled()) return lines;
    auto& state = Get();
    std::lock_guard lock(state.mutex);
    const auto now = NowMs(state);
    char line[320];
    const auto describe = [&](const char* kind, const Entry& entry) {
        std::snprintf(line, sizeof(line), "[buffer-va] %s buffer 0x%llx+0x%llx (address offset 0x%llx) created %.0f ms ago at %s%s", kind, static_cast<unsigned long long>(entry.address), static_cast<unsigned long long>(entry.bytes), static_cast<unsigned long long>(address - entry.address), now - entry.createdMs, Site(entry.site).c_str(), entry.destroyedMs >= 0 ? (", destroyed " + std::to_string(static_cast<long long>(now - entry.destroyedMs)) + " ms ago").c_str() : "");
        lines.emplace_back(line);
    };
    const Entry* below = nullptr;
    const Entry* above = nullptr;
    for (const auto& [buffer, entry] : state.live) {
        if (entry.address == 0) continue;
        if (address >= entry.address && address < entry.address + entry.bytes) describe("live", entry);
        else if (entry.address + entry.bytes <= address && (below == nullptr || entry.address > below->address)) below = &entry;
        else if (entry.address > address && (above == nullptr || entry.address < above->address)) above = &entry;
    }
    int destroyedShown = 0;
    for (auto it = state.destroyed.rbegin(); it != state.destroyed.rend() && destroyedShown < 8; ++it) {
        if (address >= it->address && address < it->address + it->bytes) {
            describe("destroyed", *it);
            ++destroyedShown;
        }
    }
    if (lines.empty()) {
        std::snprintf(line, sizeof(line), "[buffer-va] no buffer (live or among the last %zu destroyed) holds 0x%llx; %zu live buffers", state.destroyed.size(), static_cast<unsigned long long>(address), state.live.size());
        lines.emplace_back(line);
        if (below != nullptr) describe("nearest live below:", *below);
        if (above != nullptr) describe("nearest live above:", *above);
    }
    return lines;
}

}

#endif
