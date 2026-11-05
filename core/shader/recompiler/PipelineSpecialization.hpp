#ifndef CORE_SHADER_RECOMPILER_PIPELINESPECIALIZATION_HPP
#define CORE_SHADER_RECOMPILER_PIPELINESPECIALIZATION_HPP

#include <cstdint>

namespace ShaderRecompiler {

// The guest bytes [begin, end) a heap storage image addresses (its T#'s base level, one range per array slice) and
// whether the shader stores to them (ShaderInvocation::heapImageRanges).
struct HeapImageRange {
    std::uint64_t begin;
    std::uint64_t end;
    bool written;

    bool operator==(const HeapImageRange&) const = default;
};

struct PipelineSpecializationConstant {
    std::uint32_t id;
    std::uint32_t value;

    bool operator==(const PipelineSpecializationConstant&) const = default;
};

namespace PipelineSpecialization {

inline constexpr std::uint32_t CompareBase = 49152u;
inline constexpr std::uint32_t CompareWords = 6u;
inline constexpr std::uint32_t BufferBase = 0u;
inline constexpr std::uint32_t BufferWords = 4u;
inline constexpr std::uint32_t ImageBase = 1024u;
inline constexpr std::uint32_t ImageWords = 5u;
inline constexpr std::uint32_t DeferredDirectImageMode = 0xfffffff0u;

inline constexpr bool IsDeferredDirectImageMode(std::uint32_t value) {
    return value == DeferredDirectImageMode;
}
// Heap storage images (ShaderInfo::heapImages): a selector word of HeapImageMode(format) reads and writes the storage
// image as guest memory through the BDA page table, at the PS5 tiled address of its T# and in the given element
// format (an IrBufferFormat). HeapImageBase + resource * HeapImageWords holds the tiling of the selected surface:
// word 0 log2 block bytes, 1 log2 block width, 2 log2 block height (elements), 3 the mask of the T# base address
// bits that are the pipe/bank XOR (0 for the modes without one), 4..19 the 16 in-block address-bit equations
// (TextureSwizzleEquations.hpp's encoding: bits 0-11 select x, 12-23 y, 24-31 slice bits), 20 the byte offset of the
// T#'s base level in its mip chain, 21 that level's blocks per row, 22 its mip-tail placement (bit 31 set: the level
// lies in the tail block at element x bits 0-15, y bits 16-30), 23 the bytes of one array slice's mip chain.
inline constexpr std::uint32_t HeapImageModeBase = 0xfffff000u;
inline constexpr std::uint32_t HeapImageMode(std::uint32_t format) {
    return HeapImageModeBase + format;
}
inline constexpr bool IsHeapImageMode(std::uint32_t value) {
    return value >= HeapImageModeBase && value < HeapImageModeBase + 0x100u;
}
inline constexpr std::uint32_t HeapImageFormat(std::uint32_t value) {
    return value - HeapImageModeBase;
}
inline constexpr std::uint32_t HeapImageBase = 32768u;
inline constexpr std::uint32_t HeapImageWords = 24u;
inline constexpr std::uint32_t HeapImageEquationWord = 4u;
inline constexpr std::uint32_t HeapImageLevelOffsetWord = 20u;
inline constexpr std::uint32_t HeapImageBlocksPerRowWord = 21u;
inline constexpr std::uint32_t HeapImageTailWord = 22u;
inline constexpr std::uint32_t HeapImageSliceBytesWord = 23u;
inline constexpr std::uint32_t VertexBase = 4096u;
inline constexpr std::uint32_t VertexWords = 6u;
inline constexpr std::uint32_t PushDataOffset = 8188u;
inline constexpr std::uint32_t ExportBase = 8192u;
inline constexpr std::uint32_t ExportPackingBase = 8224u;
inline constexpr std::uint32_t DualSourceBlend = 8232u;
inline constexpr std::uint32_t RasterPrimitive = 8233u;
inline constexpr std::uint32_t LayerExported = 8234u;
inline constexpr std::uint32_t StencilReferenceOffset = 8235u;
inline constexpr std::uint32_t ReversedBlend = 8236u;
inline constexpr std::uint32_t DepthRemapBase = 8240u;
inline constexpr std::uint32_t DepthRemapWords = 1u;
inline constexpr std::uint32_t WorkgroupOffsetBase = 8241u;
inline constexpr std::uint32_t WorkgroupOffsetWords = 3u;
inline constexpr std::uint32_t ExportBlendBase = 8244u;
inline constexpr std::uint32_t HeapCountBase = 8256u;
inline constexpr std::uint32_t MipCountBase = 8320u;
inline constexpr std::uint32_t LevelsPastViewBase = 8384u;
// ShaderInfo::tableFormatColumns: the column's format when every record agrees, else 0 (the run-time decoder).
inline constexpr std::uint32_t TableFormatBase = 9216u;
inline constexpr std::uint32_t MaxTableFormatColumns = 32u;
inline constexpr std::uint32_t StoreQuantizeBase = 12288u;
inline constexpr std::uint32_t StoreQuantizeWords = 4u;
inline constexpr std::uint32_t DescriptorIndexBase = 16384u;
inline constexpr std::uint32_t DescriptorIndexStride = 128u;
inline constexpr std::uint32_t FoldOffsetBase = 57344u;
inline constexpr std::uint32_t UnnormalizedBaseLevelBase = 57408u;
inline constexpr std::uint32_t UnnormalizedWrapBase = 57472u;

inline constexpr std::uint32_t DescriptorIndex(std::uint32_t binding, std::uint32_t element) {
    return DescriptorIndexBase + binding * DescriptorIndexStride + element;
}

inline constexpr bool IsWorkgroupOffset(std::uint32_t id) {
    return id >= WorkgroupOffsetBase && id < WorkgroupOffsetBase + WorkgroupOffsetWords;
}

}

}

#endif
