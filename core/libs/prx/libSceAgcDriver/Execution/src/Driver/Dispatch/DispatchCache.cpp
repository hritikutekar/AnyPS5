#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include <cstdlib>

namespace AgcDriver::DriverDetail {

bool Driver::stampValidate() {
    static const bool stamp = std::getenv("APS5_DISPATCH_STAMP_VALIDATE") != nullptr;
    return stamp;
}

bool Driver::dataHits() {
    static const bool hits = std::getenv("APS5_NO_DATA_HITS") == nullptr && VulkanDevice::TemplateDataRefresh();
    return hits;
}

bool Driver::verifyDataHits() {
    static const bool verify = std::getenv("APS5_VERIFY_DATA_HITS") != nullptr;
    return verify;
}

std::size_t Driver::dispatchVariants() {
    static const std::size_t variants = [] {
        if (stampValidate()) return std::size_t{1};
        const char* text = std::getenv("APS5_DISPATCH_VARIANTS");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 4ull;
        return static_cast<std::size_t>(std::clamp<unsigned long long>(parsed, 1, MaxDispatchVariants));
    }();
    return variants;
}

bool Driver::insertCompare() {
    static const bool compare = std::getenv("APS5_INSERT_COMPARE") != nullptr;
    return compare;
}

std::size_t Driver::dispatchCacheEntries() {
    static const std::size_t entries = [] {
        const char* text = std::getenv("APS5_DISPATCH_CACHE_ENTRIES");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
        if (parsed != 0) return static_cast<std::size_t>(parsed);
        if (stampValidate()) return std::size_t{4096};
        return dispatchVariants() > 2 ? std::size_t{8192} : std::size_t{16384};
    }();
    return entries;
}

std::size_t Driver::dispatchCacheEntryCap() {
    // The dispatch cache holds 2,048 entries unless APS5_DISPATCH_CACHE_ENTRIES sets the count: in the
    // world nearly every dispatch's data changes each frame, so validating more cached variants cost
    // more than their rare hits saved (in-run A/Bs: 4,096 +1.7 % over 8,192, best243; 2,048 +0.7 % over
    // 4,096, best244; 32,768 -1.5 %, best242). APS5_NO_SMALL_DISPATCH_CACHE=1 (runtime switch) keeps the
    // default count (8,192 or 16,384).
    static const bool configured = std::getenv("APS5_DISPATCH_CACHE_ENTRIES") != nullptr;
    if (configured || Graphics::RuntimeSwitches::Set(Graphics::RuntimeSwitch::NoSmallDispatchCache)) return dispatchCacheEntries();
    return std::min<std::size_t>(dispatchCacheEntries(), 2048);
}

std::uint64_t Driver::DispatchCacheBytes() {
    static const std::uint64_t bytes = [] {
        const char* text = std::getenv("APS5_DISPATCH_CACHE_BYTES");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
        return parsed != 0 ? static_cast<std::uint64_t>(parsed) : std::uint64_t{256} << 20u;
    }();
    return bytes;
}

Driver::DispatchCacheFootprint Driver::DispatchCacheSize() {
    std::lock_guard lock(dispatchCacheMutex);
    return {dispatchCache.size(), dispatchCacheVariantBytes};
}

std::uint64_t Driver::variantBytes(const DispatchVariant& variant) {
    return sizeof(DispatchVariant) + variant.words.size() * sizeof(std::uint32_t) + variant.shared.size() * sizeof(SharedWords) + variant.runs.size() * sizeof(std::pair<std::uint64_t, std::uint64_t>) + variant.captured.size() * sizeof(ShaderRecompiler::MemoryRegion) + (variant.vertexInfo != nullptr ? sizeof(ShaderRecompiler::ShaderVertexStageInfo) : 0);
}

void Driver::accountVariant(const DispatchVariant& variant, bool added) {
    if (added) {
        ++dispatchCacheVariants;
        dispatchCacheVariantBytes += variantBytes(variant);
    } else {
        --dispatchCacheVariants;
        dispatchCacheVariantBytes -= variantBytes(variant);
    }
}

void Driver::eraseDispatchEntry(std::unordered_map<std::uint64_t, std::shared_ptr<DispatchEntry>>::iterator it) {
    for (const auto& variant : it->second->variants) accountVariant(*variant, false);
    dispatchOrder.erase(it->second->order);
    dispatchCache.erase(it);
}

}
