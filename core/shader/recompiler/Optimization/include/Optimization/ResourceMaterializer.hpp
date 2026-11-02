#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_RESOURCEMATERIALIZER_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_RESOURCEMATERIALIZER_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/SrtWalker.hpp"
#include "ImageTableAbi.hpp"
#include "BindlessImageAbi.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace ShaderRecompiler {

struct ImageTableElement {
    std::array<std::uint32_t, 8> words{};
    RdnaImageDimension dimension = RdnaImageDimension::Unknown;
    bool depthCompare = false;
    DescriptorBindingKind kind = DescriptorBindingKind::Buffers;
    std::uint32_t samplers = 0;
};

struct ImageTableSamplerElement {
    std::array<std::uint32_t, 4> words{};
    bool compare = false;
    bool point = false;
};

struct ImageTablePoisonRecord {
    std::array<std::uint32_t, 8> words{};
    std::uint32_t dwordCount = 0;
    std::uint32_t resource = 0;
    ImageTableAbi::PoisonReason reason = ImageTableAbi::PoisonReason::OutsideSnapshot;
};

struct ResolvedImageTables {
    std::vector<std::uint32_t> map;
    std::vector<ImageTableElement> elements;
    std::vector<ImageTableSamplerElement> samplers;
    std::vector<ImageTablePoisonRecord> poison;
    std::uint32_t faults = 0;
};

struct ImageTableReuseCounts {
    std::uint64_t unwritten = 0;
    std::uint64_t compared = 0;
    std::uint64_t wordsRead = 0;
    std::uint64_t shared = 0;
    std::uint64_t columns = 0;
    std::uint64_t resolutions = 0;
    std::uint64_t resolutionsShared = 0;
};

struct RuntimeImageModeMatch {
    std::optional<std::uint32_t> mode;
    ImageTableAbi::PoisonReason reason = ImageTableAbi::PoisonReason::InvalidFormat;
    std::string detail;
    bool descriptorInspected = false;
};

struct ViewLevelsPastChain {
    std::uint32_t levels = 0;
    bool base = false;
};

struct BindlessInterface {
    std::uint64_t id = 0;
    ImageResource image;
    std::vector<ImageResource> modes;
};

class ConvertedSampleRequired : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A storage image read and written as guest memory (PipelineSpecialization::HeapImageMode): the selector word and the
// tiling words of its HeapImageBase specialization.
struct HeapImageSelection {
    std::uint32_t mode = 0;
    std::array<std::uint32_t, 24> words{};
};

class ResourceMaterializer {
public:
    static ViewLevelsPastChain LevelsPastChain(const DescriptorValue& descriptor);
    static std::uint32_t EmulatedCompareState(const ShaderInfo& info, const ResourceSnapshot& snapshot, std::uint32_t index);
    void ApplyStaticInterface(IrProgram& program, bool nativeSampleOffsets = true) const;
    static std::vector<ImageResource> RuntimeImageModes(const ImageResource& image);
    static RuntimeImageModeMatch TryRuntimeImageMode(const ImageResource& image, const DescriptorValue& descriptor, std::span<const ImageResource> modes);
    static std::uint32_t RuntimeImageMode(const ImageResource& image, const DescriptorValue& descriptor, std::span<const ImageResource> modes);
    static std::uint32_t DirectImageMode(const ShaderInfo& info, IrShaderStage stage, std::uint32_t index, const DescriptorValue& descriptor, bool allowRefusal = false);
    static bool CanDeferDirectImage(const IrProgram& program, std::uint32_t index);
    static bool CanGuardUnnormalizedWrap(const IrProgram& program, std::uint32_t index);
    static void PrepareDirectImageRefusals(IrProgram& program, bool faultAbi);
    static void ValidateDirectImageRefusals(const ShaderInfo& info, IrShaderStage stage, const ResourceSnapshot& snapshot, bool allowRefusal);
    static void PrepareImageModes(ShaderInfo& info);
    // Heap storage images (SpirvTarget::heapStorageImages): ShaderInfo::heapImages marks the compute images whose
    // every access is a 32-bit texel load or store of a 2D or 2D-array address (a texel load of a stored T# is its own
    // sampled resource, so loads qualify too); the program gets the BDA page table and fault buffer their heap cases go
    // through.
    static bool HeapStorageCandidate(const IrProgram& program, std::uint32_t index);
    static void PrepareHeapStorageImages(IrProgram& program);
    // The element formats the heap cases are emitted for: 4, 8 and 16 bytes, so a texel is whole dwords and no store
    // touches a neighbour's bytes.
    static std::span<const IrBufferFormat> HeapStorageFormats();
    // The heap mode of a heap storage image's descriptor when heap storage is selected and the heap path addresses
    // it: a 2D (2D-array) surface of the image's dimension in a standard or 64 KiB XOR tile mode, base array 0, no
    // metadata compression, a heap format; the T#'s base level is the one addressed. Selected by APS5_HEAP_STORAGE=1 (SetHeapStorageSelection).
    static std::optional<HeapImageSelection> HeapImageSelectionFor(const ShaderInfo& info, std::uint32_t index, const DescriptorValue& descriptor);
    static bool HeapStorageSelected();
    static void SetHeapStorageSelection(bool selected);
    static const char* UnnormalizedSampleMismatch(const ImageResource& image);
    static std::shared_ptr<const ResolvedImageTables> ResolveImageTablesShared(const ShaderInfo& info, const ResourceSnapshot& snapshot, std::uint64_t memoKey);
    static bool NullImageDescriptor(const ImageResource& image, const DescriptorValue& descriptor);
    static ResolvedImageTables ResolveImageTables(const ShaderInfo& info, const ResourceSnapshot& snapshot, std::uint64_t memoKey = 0);
    static bool BindlessImage(const ShaderInfo& info, IrShaderStage stage, std::uint32_t resource);
    static bool UsesBindlessImages(const ShaderInfo& info, IrShaderStage stage);
    static bool RequiresPointSampler(const ImageResource& mode);
    static std::uint64_t BindlessInterfaceId(const ImageResource& image, std::span<const ImageResource> modes);
    static std::uint64_t RegisterBindlessInterface(const ImageResource& image, std::span<const ImageResource> modes);
    static void RegisterBindlessInterfaces(const ShaderInfo& info, IrShaderStage stage);
    static std::shared_ptr<const BindlessInterface> FindBindlessInterface(std::uint64_t id);
    static std::vector<BindlessImageAbi::TableRegistration> BindlessTables(const ShaderInfo& info, IrShaderStage stage, const ResourceSnapshot& snapshot);
    [[nodiscard]] IrResourcePlan ExtractPlan(const IrProgram& program) const;
    static void CompactPlan(IrResourcePlan& plan);
    void Materialize(const IrResourcePlan& program, const SrtRuntime& runtime, ResourceSnapshot& snapshot, bool allowDirectImageRefusal = false) const;
    static std::uint64_t SpecializationNanoseconds();
    static ImageTableReuseCounts TableReuse();
    static std::uint64_t EvaluateNanoseconds();
    static std::uint64_t MaterializeNanoseconds();
};

}

#endif
