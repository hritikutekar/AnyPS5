// Heap storage images (PipelineSpecialization::HeapImageMode) against the image path: each case runs one compute
// shader that loads a texel of a storage image (to an output buffer) and stores another, once through the bound image
// (written back by StorageTexture::FlushPending) and once through the heap path (the T#'s tiled guest bytes through the
// BDA page table). The guest surfaces and the loaded values must be equal byte for byte.
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "Recompiler.hpp"
#include "PipelineSpecialization.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <tuple>
#include <deque>
#include <initializer_list>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 256;
constexpr std::uint32_t InputDwords = 16;
constexpr std::size_t BlockBytes = 8u << 20u;
constexpr std::size_t BlockAlignment = 65536;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Type2DArray = 13;

alignas(256) std::array<std::uint32_t, Threads * InputDwords> Input{};
// Lane tid's loaded texel lands at dword tid * InputDwords (the shader stores it at the input's byte offset).
alignas(256) std::array<std::uint32_t, Threads * InputDwords> Output{};
// Every dispatch reads its input from a copy at an address no other dispatch used: the input is not guest memory the
// driver watches, so a buffer it uploaded before could be taken for unchanged.
std::deque<std::vector<std::uint32_t>> Inputs;

// v1 = tid * 64; v[20:23] data, v[30:33] load address, v[34:37] store address (buffer s[0:3]); image_load
// v[40:43] from s[4:11] to the output buffer s[12:15]; image_store v[20:23]. Word 6 / 12 carry the MIMG dimension.
std::vector<std::uint32_t> ShaderCode(bool array, bool store = true) {
    const std::uint32_t dim = array ? 5u : 1u;
    if (!store) {
        // Load only: the image is sampled-class and read through the driver's sampled textures on the image path.
        return {
            0x34020086u,
            0xe0381000u, 0x80001401u,
            0xe0381010u, 0x80001e01u,
            0xe0381020u, 0x80002201u,
            0xbf8c3f70u,
            0xf0001f00u | (dim << 3u), 0x0001281eu,
            0xbf8c3f70u,
            0xe0781000u, 0x80032801u,
            0xbf8c3f70u,
            0xbf810000u,
        };
    }
    return {
        0x34020086u,
        0xe0381000u, 0x80001401u,
        0xe0381010u, 0x80001e01u,
        0xe0381020u, 0x80002201u,
        0xbf8c3f70u,
        0xf0001f00u | (dim << 3u), 0x0001281eu,
        0xbf8c3f70u,
        0xe0781000u, 0x80032801u,
        0xf0201f00u | (dim << 3u), 0x00011422u,
        0xbf8c3f70u,
        0xbf810000u,
    };
}

class GuestBlock {
public:
    GuestBlock() {
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockAlignment, BlockBytes));
        Require(block != nullptr, "heap storage images: cannot allocate a guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }
    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
        std::free(block);
    }
    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;
    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
};

struct Case {
    std::string name;
    std::uint32_t format;
    std::uint32_t tile;
    std::uint32_t pipeBankXor;
    std::uint32_t swizzle;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t slices;
    std::uint32_t maxMip;
    std::uint32_t level;
};

struct Rng {
    std::uint64_t state;
    std::uint32_t Next() {
        state ^= state << 13u;
        state ^= state >> 7u;
        state ^= state << 17u;
        return static_cast<std::uint32_t>(state >> 16u);
    }
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::array<std::uint32_t, 8> TextureDescriptor(const std::uint8_t* base, const Case& c) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(base)) + c.pipeBankXor;
    const bool array = c.slices > 1u;
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (c.format << 20u) | (((c.width - 1u) & 3u) << 30u),
        ((c.width - 1u) >> 2u) | ((c.height - 1u) << 14u),
        c.swizzle | (c.level << 12u) | (c.maxMip << 16u) | (c.tile << 20u) | ((array ? Type2DArray : Type2D) << 28u),
        array ? c.slices - 1u : 0u,
        c.maxMip << 4u,
        0u,
        0u,
    };
}

std::uint64_t SurfaceBytes(const Case& c, const std::uint8_t* base) {
    const auto words = TextureDescriptor(base, c);
    const auto geometry = AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(words));
    Require(geometry.guestBytes + c.pipeBankXor <= BlockBytes, c.name + ": the surface does not fit the guest block");
    return geometry.guestBytes;
}

// Phase 0 loads from the upper half of the level and stores into the lower half; phase 1 (a second dispatch over the
// same surface) loads the texels phase 0 stored and stores into the upper half.
void FillInput(const Case& c, std::uint64_t seed, std::uint32_t phase = 0) {
    std::vector<std::array<std::uint32_t, 3>> previous;
    if (phase == 1u) {
        FillInput(c, seed, 0u);
        for (std::uint32_t tid = 0; tid < Threads; ++tid) previous.push_back({Input[tid * InputDwords + 8u], Input[tid * InputDwords + 9u], Input[tid * InputDwords + 10u]});
        seed += 0x51u;
    }
    Rng rng{seed * 0x9e3779b97f4a7c15ull + 1u};
    const auto width = std::max(c.width >> c.level, 1u);
    const auto height = std::max(c.height >> c.level, 1u);
    const auto split = height / 2u;
    std::vector<std::array<std::uint32_t, 3>> stores;
    for (std::uint32_t slice = 0; slice < c.slices; ++slice) {
        for (std::uint32_t y = phase == 0u ? split : 0u; y < (phase == 0u ? height : split); ++y) {
            for (std::uint32_t x = 0; x < width; ++x) stores.push_back({x, y, slice});
        }
    }
    for (std::size_t i = stores.size(); i > 1u; --i) std::swap(stores[i - 1u], stores[rng.Next() % i]);
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        auto* thread = Input.data() + tid * InputDwords;
        for (std::uint32_t d = 0; d < 4u; ++d) thread[d] = rng.Next() ^ (rng.Next() << 16u);
        // Some data dwords are special floats and small integers.
        if (tid % 7u == 1u) thread[0] = 0x7fc00000u;
        if (tid % 7u == 2u) thread[1] = 0x00000001u;
        if (tid % 7u == 3u) thread[2] = 0x3f800000u;
        if (tid % 7u == 4u) thread[3] = 0xbf000000u;
        if (tid % 7u == 5u) thread[0] = 0x3effffffu;
        if (split != 0u) {
            thread[4] = rng.Next() % width;
            thread[5] = rng.Next() % split;
            thread[6] = rng.Next() % c.slices;
        } else {
            thread[4] = width + tid;
            thread[5] = 0u;
            thread[6] = 0u;
        }
        if (tid < stores.size()) {
            thread[8] = stores[tid][0];
            thread[9] = stores[tid][1];
            thread[10] = stores[tid][2];
        } else {
            // Out of the level (x, y, or the slice of an array): the store is dropped.
            const auto variant = tid % (c.slices > 1u ? 3u : 2u);
            thread[8] = variant == 0u ? width + tid : tid % width;
            thread[9] = variant == 1u ? height + tid : 0u;
            thread[10] = variant == 2u ? c.slices + tid : 0u;
        }
    }
    if (phase == 1u) {
        for (std::uint32_t tid = 0; tid < Threads; ++tid) {
            auto* thread = Input.data() + tid * InputDwords;
            const auto& at = previous[(tid * 37u) % Threads];
            thread[4] = at[0];
            thread[5] = at[1];
            thread[6] = at[2];
        }
    }
}

void FillSurface(std::uint8_t* surface, std::uint64_t bytes, std::uint64_t seed) {
    Rng rng{seed * 0xbf58476d1ce4e5b9ull + 7u};
    for (std::uint64_t i = 0; i < bytes; i += 4u) {
        const auto value = rng.Next() ^ (rng.Next() << 16u);
        std::memcpy(surface + i, &value, std::min<std::uint64_t>(4u, bytes - i));
    }
}

struct Result {
    std::vector<std::uint8_t> surface;
    std::array<std::uint32_t, Threads * InputDwords> output{};
};

void Dispatch(AgcDriver::VulkanDevice& device, std::uint8_t* block, const Case& c, bool heap, bool store = true) {
    Output.fill(0xdeadbeefu);
    ShaderRecompiler::ResourceMaterializer::SetHeapStorageSelection(heap);
    const auto code = ShaderCode(c.slices > 1u, store);
    std::vector<std::uint32_t> userData(16, 0u);
    const auto& copy = Inputs.emplace_back(Input.begin(), Input.end());
    const auto input = BufferDescriptor(copy.data(), static_cast<std::uint32_t>(copy.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    const auto texture = TextureDescriptor(block, c);
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    std::copy(output.begin(), output.end(), userData.begin() + 12);
    const std::span<const std::uint32_t> words(code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(words.data()), std::as_bytes(words)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(words.data()), words, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    Require(request.target.heapStorageImages, "heap storage images: the device target has no heap cases (APS5_NO_HEAP_STORAGE is set)");
    const auto result = ShaderRecompiler::Recompile(request);
    // The texel load and the store are two resources of the same T#: the heap path binds neither image.
    const auto images = std::ranges::count_if(result.bindings, [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::GuestImages; });
    Require(heap && store ? images == 0 : images != 0 || (heap && !store), c.name + (heap ? ": the heap path still binds images" : ": the image path binds no image"));
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(words.data()));
    device.WaitIdle();
    ShaderRecompiler::ResourceMaterializer::SetHeapStorageSelection(false);
}

// One dispatch per path in `paths` over the same surface (phase 0, then phase 1), with no flush between them: an
// image's stores are still pending in it when the next dispatch runs. Then the surface lands and is compared.
Result Run(AgcDriver::VulkanDevice& device, std::uint8_t* block, const Case& c, std::uint64_t seed, std::initializer_list<bool> paths) {
    const auto bytes = SurfaceBytes(c, block);
    FillSurface(block, bytes + c.pipeBankXor, seed);
    std::uint32_t phase = 0;
    for (const bool heap : paths) {
        FillInput(c, seed, phase++);
        Dispatch(device, block, c, heap);
    }
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(block), bytes + c.pipeBankXor);
    AgcDriver::Graphics::StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(block), bytes + c.pipeBankXor, nullptr, "test");
    device.WaitIdle();
    Result run;
    run.surface.assign(block, block + bytes + c.pipeBankXor);
    run.output = Output;
    return run;
}

struct PackedResult {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint32_t> loads;
};

// The reflection probe's HDR chain (best642/695: 256x256 fmt 71 SW_4KB_S, 8 mips at 0x502c6b4000): writers store each
// level through a view of it (base level L), then load-only consumers (sampled textures on the image path) or
// load-and-store consumers read every level back.
PackedResult RunChain(AgcDriver::VulkanDevice& device, std::uint8_t* block, std::uint64_t seed, bool writerHeap, bool storeConsumers) {
    Case chain{"256x256 fmt 71 SW_4KB_S 8 mips", 71u, 0x05u, 0u, 0xfacu, 256u, 256u, 1u, 7u, 0u};
    const auto bytes = SurfaceBytes(chain, block);
    FillSurface(block, bytes, seed);
    for (std::uint32_t level = 0; level < 8u; ++level) {
        auto c = chain;
        c.level = level;
        FillInput(c, seed + level, 0u);
        Dispatch(device, block, c, writerHeap);
    }
    PackedResult result;
    for (std::uint32_t level = 0; level < 8u; ++level) {
        auto c = chain;
        c.level = level;
        FillInput(c, seed + level, 1u);
        Dispatch(device, block, c, false, storeConsumers);
        for (std::uint32_t tid = 0; tid < Threads; ++tid) result.loads.insert(result.loads.end(), Output.begin() + tid * InputDwords, Output.begin() + tid * InputDwords + 4u);
    }
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(block), bytes);
    AgcDriver::Graphics::StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(block), bytes, nullptr, "test");
    device.WaitIdle();
    result.bytes.assign(block, block + bytes);
    return result;
}

// The reflection probe's packed HDR levels (best483): three single-level fmt 71 SW_4KB_S surfaces whose 4 KiB padded
// extents overlap, each storing only its element extent: 8x8 at +0x600 (0x200 bytes), 16x16 at +0x800 (0x800), 32x32 at
// +0x1000 (0x2000). Writers store each surface (the image or the heap path), then consumers through `consumerHeap` load
// what was stored; the region's bytes and every load are compared.

PackedResult RunPacked(AgcDriver::VulkanDevice& device, std::uint8_t* block, std::uint64_t seed, bool writerHeap, bool consumerHeap) {
    constexpr std::uint32_t RegionBytes = 0x3000;
    const std::array<std::pair<std::uint32_t, Case>, 3> surfaces{{
        {0x1000u, {"32x32 +0x1000", 71u, 0x05u, 0u, 0xfacu, 32u, 32u, 1u, 0u, 0u}},
        {0x800u, {"16x16 +0x800", 71u, 0x05u, 0u, 0xfacu, 16u, 16u, 1u, 0u, 0u}},
        {0x600u, {"8x8 +0x600", 71u, 0x05u, 0u, 0xfacu, 8u, 8u, 1u, 0u, 0u}},
    }};
    FillSurface(block, RegionBytes, seed);
    for (const auto& [offset, c] : surfaces) {
        FillInput(c, seed + offset, 0u);
        Dispatch(device, block + offset, c, writerHeap);
    }
    PackedResult result;
    for (const auto& [offset, c] : surfaces) {
        FillInput(c, seed + offset, 1u);
        Dispatch(device, block + offset, c, consumerHeap);
        for (std::uint32_t tid = 0; tid < Threads; ++tid) result.loads.insert(result.loads.end(), Output.begin() + tid * InputDwords, Output.begin() + tid * InputDwords + 4u);
    }
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(block), RegionBytes);
    AgcDriver::Graphics::StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(block), RegionBytes, nullptr, "test");
    device.WaitIdle();
    result.bytes.assign(block, block + RegionBytes);
    return result;
}

bool ComparePacked(const char* name, const PackedResult& reference, const PackedResult& other) {
    std::size_t bytes = 0, first = SIZE_MAX, last = 0, loads = 0;
    for (std::size_t i = 0; i < reference.bytes.size(); ++i) {
        if (reference.bytes[i] == other.bytes[i]) continue;
        ++bytes;
        first = std::min(first, i);
        last = i;
    }
    for (std::size_t i = 0; i < reference.loads.size(); ++i) loads += reference.loads[i] != other.loads[i];
    if (bytes == 0u && loads == 0u) {
        std::printf("  ok   packed probe levels: %s\n", name);
        return true;
    }
    std::printf("  FAIL packed probe levels: %s: %zu region bytes differ", name, bytes);
    if (bytes != 0u) std::printf(" (in +0x%zx..+0x%zx)", first, last);
    std::printf(", %zu loaded dwords differ (of %zu: 32x32, 16x16, 8x8 consumers in that order)\n", loads, reference.loads.size());
    return false;
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

bool Compare(const Case& c, const Result& image, const Result& heap) {
    std::size_t surfaceDiffs = 0;
    std::size_t first = image.surface.size();
    for (std::size_t i = 0; i < image.surface.size(); ++i) {
        if (image.surface[i] == heap.surface[i]) continue;
        if (surfaceDiffs++ == 0u) first = i;
    }
    std::size_t outputDiffs = 0;
    std::size_t firstOutput = 0;
    for (std::size_t i = 0; i < image.output.size(); ++i) {
        if (image.output[i] == heap.output[i]) continue;
        if (outputDiffs++ == 0u) firstOutput = i;
    }
    std::size_t unwritten = 0;
    for (std::uint32_t tid = 0; tid < Threads; ++tid) unwritten += image.output[tid * InputDwords] == 0xdeadbeefu && image.output[tid * InputDwords + 1u] == 0xdeadbeefu;
    if (unwritten != 0u) std::printf("  note %s: %zu lanes of the reference wrote no loaded texel\n", c.name.c_str(), unwritten);
    if (surfaceDiffs == 0u && outputDiffs == 0u) {
        std::printf("  ok   %s\n", c.name.c_str());
        return true;
    }
    std::printf("  FAIL %s: %zu surface bytes differ", c.name.c_str(), surfaceDiffs);
    if (surfaceDiffs != 0u) {
        const auto word = first & ~std::size_t{3};
        std::uint32_t a = 0, b = 0;
        std::memcpy(&a, image.surface.data() + word, 4u);
        std::memcpy(&b, heap.surface.data() + word, 4u);
        std::printf(" (first at byte 0x%zx: image %s heap %s)", first, Hex(a).c_str(), Hex(b).c_str());
    }
    std::printf(", %zu loaded dwords differ", outputDiffs);
    if (outputDiffs != 0u) {
        const auto tid = firstOutput / InputDwords;
        const auto* thread = Input.data() + tid * InputDwords;
        std::printf(" (first: thread %zu component %zu at (%u, %u, %u): image %s heap %s)", tid, firstOutput % InputDwords, thread[4], thread[5], thread[6], Hex(image.output[firstOutput]).c_str(), Hex(heap.output[firstOutput]).c_str());
    }
    std::printf("\n");
    return false;
}

std::vector<Case> Cases() {
    struct Format {
        const char* name;
        std::uint32_t value;
    };
    constexpr std::array<Format, 18> Formats{{
        {"32_UINT", 20u}, {"32_SINT", 21u}, {"32_FLOAT", 22u}, {"16_16_UNORM", 23u}, {"16_16_UINT", 27u}, {"16_16_FLOAT", 29u},
        {"11_11_10_FLOAT", 36u}, {"10_10_10_2_UNORM", 50u}, {"8_8_8_8_UNORM", 56u}, {"8_8_8_8_SNORM", 57u}, {"8_8_8_8_UINT", 60u}, {"32_32_UINT", 62u},
        {"32_32_FLOAT", 64u}, {"16_16_16_16_UNORM", 65u}, {"16_16_16_16_UINT", 69u}, {"16_16_16_16_FLOAT", 71u},
        {"32_32_32_32_UINT", 75u}, {"32_32_32_32_FLOAT", 77u},
    }};
    struct Tile {
        const char* name;
        std::uint32_t value;
        std::uint32_t pipeBankXor;
    };
    constexpr std::array<Tile, 6> Tiles{{
        {"SW_4KB_S", 0x05u, 0u}, {"SW_64KB_S", 0x09u, 0u}, {"SW_64KB_Z_X", 0x18u, 0x5600u}, {"SW_64KB_S_X", 0x19u, 0x0300u},
        {"SW_64KB_D_X", 0x1au, 0u}, {"SW_64KB_R_X", 0x1bu, 0xf100u},
    }};
    std::vector<Case> cases;
    for (const auto& format : Formats) {
        for (const auto& tile : Tiles) {
            cases.push_back({std::string(format.name) + " " + tile.name + " 2D 200x70", format.value, tile.value, tile.pipeBankXor, 0xfacu, 200u, 70u, 1u, 0u, 0u});
        }
    }
    for (const auto& format : {Formats[2], Formats[8], Formats[15], Formats[17]}) {
        cases.push_back({std::string(format.name) + " SW_64KB_R_X 480x270 mips 0..8 level 0", format.value, 0x1bu, 0x2200u, 0xfacu, 480u, 270u, 1u, 8u, 0u});
        cases.push_back({std::string(format.name) + " SW_64KB_R_X 480x270 mips 0..8 level 2", format.value, 0x1bu, 0x2200u, 0xfacu, 480u, 270u, 1u, 8u, 2u});
        cases.push_back({std::string(format.name) + " SW_64KB_R_X 480x270 mips 0..8 level 6 (tail)", format.value, 0x1bu, 0x2200u, 0xfacu, 480u, 270u, 1u, 8u, 6u});
        cases.push_back({std::string(format.name) + " SW_64KB_R_X 480x270 mips 0..8 level 8 (1x1, loads out of the level)", format.value, 0x1bu, 0x2200u, 0xfacu, 480u, 270u, 1u, 8u, 8u});
        cases.push_back({std::string(format.name) + " SW_4KB_S 120x90 mips 0..6 level 3", format.value, 0x05u, 0u, 0xfacu, 120u, 90u, 1u, 6u, 3u});
        cases.push_back({std::string(format.name) + " SW_64KB_S_X 2D array 96x40x3", format.value, 0x19u, 0x0300u, 0xfacu, 96u, 40u, 3u, 0u, 0u});
        cases.push_back({std::string(format.name) + " SW_64KB_R_X swizzle ZYXW", format.value, 0x1bu, 0u, 0xf2eu, 200u, 70u, 1u, 0u, 0u});
        cases.push_back({std::string(format.name) + " SW_64KB_R_X swizzle X001", format.value, 0x1bu, 0u, 0x24cu, 200u, 70u, 1u, 0u, 0u});
    }
    return cases;
}

}

int main(int argc, char** argv) {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (argc > 1 && std::string(argv[1]) == "--chain") {
            GuestBlock imageBlock;
            GuestBlock heapBlock;
            bool equal = true;
            for (std::uint64_t seed = 1; seed <= 3; ++seed) {
                for (const bool storeConsumers : {false, true}) {
                    const auto reference = RunChain(*device, imageBlock.Data(), seed, false, storeConsumers);
                    const auto heap = RunChain(*device, heapBlock.Data(), seed, true, storeConsumers);
                    equal = ComparePacked(storeConsumers ? "chain: heap writers, image load+store consumers" : "chain: heap writers, image load-only (sampled) consumers", reference, heap) && equal;
                }
            }
            std::printf("heap storage images, probe chain: %s\n", equal ? "equal to the image path" : "DIFFERENT from the image path");
            return equal ? 0 : 1;
        }
        if (argc > 1 && std::string(argv[1]) == "--packed") {
            GuestBlock imageBlock;
            GuestBlock heapBlock;
            bool equal = true;
            for (std::uint64_t seed = 1; seed <= 3; ++seed) {
                const auto reference = RunPacked(*device, imageBlock.Data(), seed, false, false);
                equal = ComparePacked("heap writers, image consumers", reference, RunPacked(*device, heapBlock.Data(), seed, true, false)) && equal;
                equal = ComparePacked("heap writers, heap consumers", reference, RunPacked(*device, heapBlock.Data(), seed, true, true)) && equal;
                equal = ComparePacked("image writers, heap consumers", reference, RunPacked(*device, heapBlock.Data(), seed, false, true)) && equal;
            }
            std::printf("heap storage images, packed probe levels: %s\n", equal ? "equal to the image path" : "DIFFERENT from the image path");
            return equal ? 0 : 1;
        }
        const bool mixed = argc > 1 && std::string(argv[1]) == "--mixed";
        const std::string only = argc > (mixed ? 2 : 1) ? argv[mixed ? 2 : 1] : "";
        GuestBlock imageBlock;
        GuestBlock heapBlock;
        std::size_t failed = 0;
        std::size_t run = 0;
        std::uint64_t seed = 1;
        for (const auto& c : Cases()) {
            ++seed;
            if (!only.empty() && c.name.find(only) == std::string::npos) continue;
            ++run;
            if (!mixed) {
                const auto image = Run(*device, imageBlock.Data(), c, seed, {false});
                const auto heap = Run(*device, heapBlock.Data(), c, seed, {true});
                if (!Compare(c, image, heap)) ++failed;
                continue;
            }
            // The second dispatch loads what the first stored, through the other path or the same one.
            const auto reference = Run(*device, imageBlock.Data(), c, seed, {false, false});
            bool equal = true;
            for (const auto& [first, second, label] : {std::tuple{false, true, "image then heap"}, std::tuple{true, false, "heap then image"}, std::tuple{true, true, "heap then heap"}}) {
                auto named = c;
                named.name += std::string(": ") + label;
                equal = Compare(named, reference, Run(*device, heapBlock.Data(), c, seed, {first, second})) && equal;
            }
            // 32_UINT stores its data dword unchanged: the second dispatch's loads must return what the first stored.
            if (c.format == 20u && c.level == 0u && c.slices == 1u) {
                const auto result = Run(*device, heapBlock.Data(), c, seed, {false, true});
                FillInput(c, seed, 0u);
                std::size_t hits = 0;
                for (std::uint32_t tid = 0; tid < Threads; ++tid) hits += result.output[tid * InputDwords] == Input[((tid * 37u) % Threads) * InputDwords];
                std::printf("  %s: %zu of %u second-dispatch loads return the first dispatch's stores (image then heap)\n", c.name.c_str(), hits, Threads);
                equal = equal && hits == Threads;
            }
            if (!equal) ++failed;
        }
        std::printf("heap storage images: %zu of %zu cases equal the image path\n", run - failed, run);
        return failed == 0u ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
