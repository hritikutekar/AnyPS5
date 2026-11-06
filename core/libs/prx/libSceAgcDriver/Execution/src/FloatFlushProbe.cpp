#include "prx/libSceAgcDriver/Execution/include/FloatFlushProbe.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace AgcDriver {
namespace {

using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Inputs = 4;
constexpr std::uint32_t Results = 19;
alignas(256) std::array<std::uint32_t, Threads * Inputs> Input{};
alignas(256) std::array<std::uint32_t, Threads * Results> Output{};

alignas(256) constexpr std::array<std::uint32_t, 46> MixedCode{
    0x34020084u, 0x160600ffu, 0x0000004cu, 0xe0381000u, 0x80000401u, 0xbf8c3f70u, 0x06140b04u, 0x08160905u,
    0x10180b04u, 0x0e1a0b04u, 0xd54b000eu, 0x041a0b04u, 0x7e1e0306u, 0x561e0b04u, 0x1e200b04u, 0xd5540011u,
    0x041a0b04u, 0xd5570012u, 0x041a0b04u, 0x7e264904u, 0x7e284505u, 0x7e2a8104u, 0x7c020b04u, 0xd5010016u,
    0x01a90280u, 0x7e2e1b04u, 0x7e307f05u, 0xd5080119u, 0x42020b04u, 0xd55f001au, 0x041a0b04u, 0xd550001bu,
    0x041a0b04u, 0xd550001cu, 0x03ca0b04u, 0xe0781000u, 0x80010a03u, 0xe0781010u, 0x80010e03u, 0xe0781020u,
    0x80011203u, 0xe0781030u, 0x80011603u, 0xe07c1040u, 0x80011a03u, 0xbf810000u,
};

constexpr std::uint32_t MixedRows[Threads][Inputs] = {
    {0x00000001u, 0x3f800000u, 0x00000000u, 0x00000000u},
    {0x80400000u, 0x3f800000u, 0x00800000u, 0x00000000u},
    {0x3f800000u, 0x00012345u, 0x80000001u, 0x00000000u},
    {0x007fffffu, 0x007fffffu, 0x3f800000u, 0x00000000u},
    {0x3f7fffffu, 0x00800000u, 0x00000000u, 0x00000000u},
    {0xbf7fffffu, 0x00800000u, 0x80000000u, 0x00000000u},
    {0x3f7fffffu, 0x00800001u, 0x00000001u, 0x00000000u},
    {0x3f000000u, 0x01000000u, 0x80000001u, 0x00000000u},
    {0x00800000u, 0x80800001u, 0x00800000u, 0x00000000u},
    {0x00800001u, 0x00800000u, 0x3f800000u, 0x00000000u},
    {0x00000000u, 0x80000001u, 0x00000001u, 0x00000000u},
    {0x80000000u, 0x00000001u, 0x807fffffu, 0x00000000u},
    {0x7f800000u, 0x00000001u, 0x3f800000u, 0x00000000u},
    {0x00000001u, 0xff800000u, 0x00000000u, 0x00000000u},
    {0x7fc00000u, 0x00000001u, 0x00000001u, 0x00000000u},
    {0x40000000u, 0x00400000u, 0x80800000u, 0x00000000u},
    {0x3f800000u, 0x3f800000u, 0x80000001u, 0x00000000u},
    {0xbf800000u, 0x00000001u, 0x3f000000u, 0x00000000u},
    {0x34000000u, 0x34000000u, 0x00000000u, 0x00000000u},
    {0x1f800000u, 0x1f800000u, 0x00000000u, 0x00000000u},
    {0x1fffffffu, 0x1fffffffu, 0x00800000u, 0x00000000u},
    {0x20000000u, 0x1f000000u, 0x00000000u, 0x00000000u},
    {0x20000000u, 0x1f7fffffu, 0x00000000u, 0x00000000u},
    {0x20000001u, 0x1f7fffffu, 0x00000000u, 0x00000000u},
    {0xa0000000u, 0x1f7fffffu, 0x80000000u, 0x00000000u},
    {0x3f800000u, 0xbf800000u, 0x00000001u, 0x00000000u},
    {0x00800000u, 0x3f000000u, 0x00000000u, 0x00000000u},
    {0x00800000u, 0x3f7fffffu, 0x00000001u, 0x00000000u},
    {0x41200000u, 0x00000003u, 0x3dcccccdu, 0x00000000u},
    {0xc0490fdbu, 0x80000002u, 0x40490fdbu, 0x00000000u},
    {0x80000001u, 0x80000001u, 0x80000001u, 0x00000000u},
    {0x00000001u, 0x00000001u, 0x00000001u, 0x00000000u}
};

alignas(256) constexpr std::array<std::uint32_t, 30> TinyCode{
    0x34020084u, 0x34040086u, 0xe0381000u, 0x80000401u, 0xbf8c3f70u, 0x10140b04u, 0x0e160b04u, 0xd54b000cu,
    0x041a0b04u, 0x7e1a0306u, 0x561a0b04u, 0x581c0d04u, 0x3f7ffffeu, 0x5a1e0b04u, 0x80000000u, 0xcc200010u,
    0x041a0b04u, 0xd5080011u, 0x22020b04u, 0xd5400012u, 0x041a0b04u, 0x7e260306u, 0x0c260b04u, 0xe0781000u,
    0x80010a02u, 0xe0781010u, 0x80010e02u, 0xe0741020u, 0x80011202u, 0xbf810000u,
};

constexpr std::uint32_t TinyRows[Threads][Inputs] = {
    {0x00800001u, 0x3f7ffffeu, 0x00000000u, 0x00000000u},
    {0x80800001u, 0x3f7ffffeu, 0x80000000u, 0x00000000u},
    {0x3f7ffffeu, 0x00800001u, 0x00000000u, 0x00000000u},
    {0x008003e6u, 0x3f7ff834u, 0x00000000u, 0x00000000u},
    {0x00f80000u, 0x3f042108u, 0x80000000u, 0x00000000u},
    {0x80f80000u, 0x3f042108u, 0x00000000u, 0x00000000u},
    {0x00800f95u, 0x3f7fe0d9u, 0x00000000u, 0x00000000u},
    {0x00800001u, 0x3f7ffffdu, 0x00000000u, 0x00000000u},
    {0x00ffffffu, 0x3f000000u, 0x00000000u, 0x00000000u},
    {0x008005a8u, 0x3f7ff4b0u, 0x00000000u, 0x00000000u},
    {0x00f7ffffu, 0x3f042108u, 0x00000000u, 0x00000000u},
    {0x00ffffffu, 0x3effffffu, 0x00000000u, 0x00000000u},
    {0x008005a9u, 0x3f7ff4aeu, 0x00000000u, 0x00000000u},
    {0x00800001u, 0x3f7ffffeu, 0x00000001u, 0x00000000u},
    {0x007fffffu, 0x3f800001u, 0x00000000u, 0x00000000u},
    {0x00800000u, 0x3f7fffffu, 0x00000000u, 0x00000000u},
    {0x80800001u, 0x3f7ffffeu, 0x80000000u, 0x00000000u},
    {0x00800001u, 0x3f7ffffeu, 0x00000000u, 0x00000000u},
    {0xbf7ffffeu, 0x00800001u, 0x80000000u, 0x00000000u},
    {0x808003e6u, 0x3f7ff834u, 0x80000000u, 0x00000000u},
    {0x80f80000u, 0x3f042108u, 0x00000000u, 0x00000000u},
    {0x00f80000u, 0x3f042108u, 0x80000000u, 0x00000000u},
    {0x80800f95u, 0x3f7fe0d9u, 0x80000000u, 0x00000000u},
    {0x80800001u, 0x3f7ffffdu, 0x80000000u, 0x00000000u},
    {0x80ffffffu, 0x3f000000u, 0x80000000u, 0x00000000u},
    {0x808005a8u, 0x3f7ff4b0u, 0x80000000u, 0x00000000u},
    {0x80f7ffffu, 0x3f042108u, 0x80000000u, 0x00000000u},
    {0x80ffffffu, 0x3effffffu, 0x80000000u, 0x00000000u},
    {0x808005a9u, 0x3f7ff4aeu, 0x80000000u, 0x00000000u},
    {0x80800001u, 0x3f7ffffeu, 0x80000001u, 0x00000000u},
    {0x807fffffu, 0x3f800001u, 0x80000000u, 0x00000000u},
    {0x80800000u, 0x3f7fffffu, 0x80000000u, 0x00000000u}
};

alignas(256) constexpr std::array<std::uint32_t, 28> MixedTinyCode{
    0x34020084u, 0x34040086u, 0xe0381000u, 0x80000401u, 0xbf8c3f70u, 0xcc20000au, 0x141a0b04u, 0xcc20000bu,
    0x0c1a0905u, 0xcc20100cu, 0x141a0b04u, 0xcc20080du, 0x0c1a0905u, 0xcc20400eu, 0x141e0b04u, 0xcc20400fu,
    0x0c1e0905u, 0x7e1002ffu, 0x3f7ffffeu, 0xcc204010u, 0x041e1104u, 0xcc206011u, 0x041e1104u, 0xe0781000u,
    0x80010a02u, 0xe0781010u, 0x80010e02u, 0xbf810000u,
};

constexpr std::uint32_t MixedTinyRows[Threads][Inputs] = {
    {0x00801002u, 0x00003bffu, 0x00000000u, 0x00000000u},
    {0x80801002u, 0x00003bffu, 0x80000000u, 0x80008000u},
    {0x00801000u, 0x3bff3bffu, 0x00000000u, 0x00000000u},
    {0x00801001u, 0x3bff3bffu, 0x00000000u, 0x00000000u},
    {0x00801002u, 0x3bff3bffu, 0x00000000u, 0x00000000u},
    {0x00801003u, 0x3bff3bffu, 0x00000000u, 0x00000000u},
    {0x80801000u, 0x3bff3bffu, 0x80000000u, 0x80008000u},
    {0x80801001u, 0x3bff3bffu, 0x80000000u, 0x80008000u},
    {0x80801002u, 0x3bff3bffu, 0x80000000u, 0x80008000u},
    {0x80801003u, 0x3bff3bffu, 0x80000000u, 0x80008000u},
    {0x00801000u, 0xbbffbbffu, 0x80000000u, 0x80008000u},
    {0x00801001u, 0xbbffbbffu, 0x80000000u, 0x80008000u},
    {0x00801002u, 0xbbffbbffu, 0x80000000u, 0x80008000u},
    {0x00801003u, 0xbbffbbffu, 0x80000000u, 0x80008000u},
    {0x00800001u, 0x3bff3bffu, 0x00000000u, 0x00000000u},
    {0x80800001u, 0x3bff3bffu, 0x80000000u, 0x80008000u},
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

bool IsNan(std::uint32_t value) {
    return (value & 0x7fffffffu) > 0x7f800000u;
}

std::vector<std::uint32_t> Run(VulkanDevice& device, ShaderRecompiler::SpirvTarget target, bool hardwareFlush, const std::optional<ShaderRecompiler::ShaderFloatMode>& floatMode, std::span<const std::uint32_t> code, const std::uint32_t (*rows)[Inputs]) {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) std::copy(std::begin(rows[tid]), std::end(rows[tid]), &Input[tid * Inputs]);
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    target.float32FlushToZero = hardwareFlush;
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        target,
        {0, 0, 0, 128}
    };
    request.useCache = false;
    request.context.floatMode = floatMode;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    return {Output.begin(), Output.end()};
}


// APS5_PROBE_HARDWARE_FLOAT=1: one program runs rcp, rsq, sqrt, log, exp, fract, floor, ceil, trunc, rndne, sin, cos,
// med3, output modifiers, add, mul and fma on specials and random operands, with the exact forms and with
// SpirvTarget::hardwareNanResults/hardwareFloatOps; the differences are logged by operation and kind.
alignas(256) std::array<std::uint32_t, 41> HardwareFloatCode{};
alignas(256) std::uint32_t HardwareFloatRows[Threads][Inputs]{};
constexpr std::array<const char*, Results> HardwareFloatNames{"rcp", "rsq", "sqrt", "log", "exp", "fract", "floor", "ceil", "trunc", "rndne", "sin", "cos", "med3", "omod*2", "omod*4", "omod/2", "add", "mul", "fma"};

void BuildHardwareFloatCode() {
    std::size_t at = 0;
    for (std::size_t index = 0; index < 6; ++index) HardwareFloatCode[at++] = MixedCode[index];
    std::uint32_t destination = 10;
    for (const std::uint32_t opcode : {0x2au, 0x2eu, 0x33u, 0x27u, 0x25u, 0x20u, 0x24u, 0x22u, 0x21u, 0x23u, 0x35u, 0x36u}) HardwareFloatCode[at++] = 0x7e000000u | destination++ << 17u | opcode << 9u | 0x104u;
    HardwareFloatCode[at++] = 0xd5570000u | destination++;
    HardwareFloatCode[at++] = 0x041a0b04u;
    for (const std::uint32_t omod : {1u, 2u, 3u}) {
        HardwareFloatCode[at++] = 0xd5080000u | destination++;
        HardwareFloatCode[at++] = 0x00000b04u | omod << 27u;
    }
    HardwareFloatCode[at++] = 0x06000b04u | destination++ << 17u;
    HardwareFloatCode[at++] = 0x10000b04u | destination++ << 17u;
    HardwareFloatCode[at++] = 0xd54b0000u | destination++;
    HardwareFloatCode[at++] = 0x041a0b04u;
    for (std::size_t index = 35; index < MixedCode.size(); ++index) HardwareFloatCode[at++] = MixedCode[index];
}

void FillHardwareFloatRows(std::uint32_t batch, std::uint64_t& random) {
    constexpr std::array<std::uint32_t, 40> Specials{
        0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u, 0x7fc12345u, 0x7f812345u, 0xff812345u, 0x00000001u, 0x80000001u, 0x007fffffu, 0x807fffffu, 0x00800000u, 0x80800000u, 0x00800001u,
        0x3f800000u, 0xbf800000u, 0x3f7fffffu, 0x3f800001u, 0x3e800000u, 0x3f000000u, 0x3f400000u, 0xbe800000u, 0xbf000000u, 0x40000000u, 0x40800000u, 0x1e3ce508u, 0x60ad78ecu, 0x7f7fffffu, 0xff7fffffu, 0x42fe0000u,
        0xc2fc0000u, 0xc3000000u, 0x43000000u, 0x4b000000u, 0x4b7fffffu, 0x3fc00000u, 0xbfc00000u, 0x2f800000u};
    const auto next = [&] {
        random = random * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(random >> 32u);
    };
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        for (std::uint32_t input = 0; input < Inputs; ++input) {
            std::uint32_t value = next();
            if (batch < 16u) value = Specials[value % Specials.size()];
            else if (batch % 2u == 0u) value = (value & 0x807fffffu) | (107u + (value >> 23u & 0xffu) * 40u / 256u) << 23u;
            HardwareFloatRows[tid][input] = value;
        }
    }
}

std::int64_t Ordered(std::uint32_t bits) {
    return (bits & 0x80000000u) != 0u ? -static_cast<std::int64_t>(bits & 0x7fffffffu) : static_cast<std::int64_t>(bits);
}

float AsFloat(std::uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}

void LogHardwareFloatProbe(VulkanDevice& device, const ShaderRecompiler::SpirvTarget& target) {
    struct Kind { std::uint64_t compared = 0, same = 0, payload = 0, zeroSign = 0, nearby = 0, nanness = 0, far = 0; std::int64_t worstUlp = 0; float worstAbsolute = 0.0f; std::uint32_t exact = 0, hardware = 0, a = 0, b = 0, c = 0; };
    BuildHardwareFloatCode();
    auto exact = target;
    exact.hardwareNanResults = false;
    exact.hardwareFloatOps = false;
    auto native = target;
    native.hardwareNanResults = true;
    native.hardwareFloatOps = true;
    const std::array<std::optional<ShaderRecompiler::ShaderFloatMode>, 3> modes{std::nullopt, ShaderRecompiler::ShaderFloatMode{0xc0u, true, false, false}, ShaderRecompiler::ShaderFloatMode{0x00u, false, true, false}};
    for (std::size_t mode = 0; mode < modes.size(); ++mode) {
        std::array<Kind, Results> kinds{};
        std::uint64_t random = 0x1234567u + mode;
        for (std::uint32_t batch = 0; batch < 64u; ++batch) {
            FillHardwareFloatRows(batch, random);
            const auto reference = Run(device, exact, target.float32FlushToZero, modes[mode], HardwareFloatCode, HardwareFloatRows);
            const auto result = Run(device, native, target.float32FlushToZero, modes[mode], HardwareFloatCode, HardwareFloatRows);
            for (std::size_t index = 0; index < reference.size(); ++index) {
                auto& kind = kinds[index % Results];
                const auto left = reference[index];
                const auto right = result[index];
                ++kind.compared;
                if (left == right) { ++kind.same; continue; }
                if (IsNan(left) && IsNan(right)) { ++kind.payload; continue; }
                if ((left & 0x7fffffffu) == 0u && (right & 0x7fffffffu) == 0u) { ++kind.zeroSign; continue; }
                if (IsNan(left) != IsNan(right)) ++kind.nanness;
                const auto ulp = std::llabs(Ordered(left) - Ordered(right));
                if (!IsNan(left) && !IsNan(right) && ulp <= 16) ++kind.nearby;
                else if (!IsNan(left) && !IsNan(right)) ++kind.far;
                if (ulp > kind.worstUlp) {
                    const auto* row = HardwareFloatRows[index / Results];
                    kind.worstUlp = ulp;
                    kind.exact = left;
                    kind.hardware = right;
                    kind.a = row[0];
                    kind.b = row[1];
                    kind.c = row[2];
                }
                if (!IsNan(left) && !IsNan(right)) kind.worstAbsolute = std::max(kind.worstAbsolute, std::fabs(AsFloat(left) - AsFloat(right)));
            }
        }
        for (std::size_t index = 0; index < Results; ++index) {
            const auto& kind = kinds[index];
            std::fprintf(stderr, "[hw-float-probe] mode %zu %-7s compared %llu same %llu payload-only %llu zero-sign %llu within-16-ulp %llu far %llu nan-vs-number %llu; worst ulp %lld abs %g (exact %08x hardware %08x for %08x %08x %08x)\n", mode, HardwareFloatNames[index], static_cast<unsigned long long>(kind.compared), static_cast<unsigned long long>(kind.same), static_cast<unsigned long long>(kind.payload), static_cast<unsigned long long>(kind.zeroSign), static_cast<unsigned long long>(kind.nearby), static_cast<unsigned long long>(kind.far), static_cast<unsigned long long>(kind.nanness), static_cast<long long>(kind.worstUlp), static_cast<double>(kind.worstAbsolute), kind.exact, kind.hardware, kind.a, kind.b, kind.c);
        }
    }
}


FloatFlushProbeResult ProbeFloat32FlushToZero(VulkanDevice& device, const ShaderRecompiler::SpirvTarget& target) {
    FloatFlushProbeResult result;
    const std::array<std::optional<ShaderRecompiler::ShaderFloatMode>, 3> modes{std::nullopt, ShaderRecompiler::ShaderFloatMode{0xc0u, true, false, false}, ShaderRecompiler::ShaderFloatMode{0x00u, false, true, false}};
    const std::array<std::pair<std::span<const std::uint32_t>, const std::uint32_t (*)[Inputs]>, 3> programs{{{MixedCode, MixedRows}, {TinyCode, TinyRows}, {MixedTinyCode, MixedTinyRows}}};
    for (const auto& mode : modes) {
        for (const auto& [code, rows] : programs) {
            const auto software = Run(device, target, false, mode, code, rows);
            const auto hardware = Run(device, target, true, mode, code, rows);
            for (std::size_t index = 0; index < software.size(); ++index) {
                ++result.compared;
                if (software[index] != hardware[index] && !(IsNan(software[index]) && IsNan(hardware[index]))) ++result.mismatches;
            }
        }
    }
    return result;
}

}
