#include "Optimization/TableFormats.hpp"
#include "PipelineSpecialization.hpp"
#include <algorithm>
#include <cstdlib>

namespace ShaderRecompiler {

namespace {

bool FlatSlot(const IrValue* value, std::uint32_t& slot) {
    const auto* read = value->Resolve();
    if (read->Opcode() != IrOpcode::ReadConst || read->ArgumentCount() != 2u) return false;
    const auto* index = read->Argument(1)->Resolve();
    if (!index->HasImmediate()) return false;
    slot = index->ImmediateU32();
    return true;
}

}

std::optional<TableFormatColumn> TableFormatColumnOf(const IrProgram& program, const IrValue& load) {
    if (load.ArgumentCount() == 0u) return std::nullopt;
    const auto* resource = load.Argument(0)->Resolve();
    if (resource->Opcode() != IrOpcode::GetBufferResource || resource->ArgumentCount() != 4u) return std::nullopt;
    const auto* word = resource->Argument(3)->Resolve();
    if (word->Opcode() != IrOpcode::ReadConstBuffer || word->ArgumentCount() != 2u) return std::nullopt;
    const auto* table = word->Argument(0)->Resolve();
    if (table->Opcode() != IrOpcode::GetBufferResource || table->ArgumentCount() != 4u) return std::nullopt;
    const auto* index = word->Argument(1)->Resolve();
    if (index->Opcode() != IrOpcode::IMul32 || index->ArgumentCount() != 2u) return std::nullopt;
    const auto* stride = index->Argument(1)->Resolve();
    if (!stride->HasImmediate() || stride->ImmediateU32() == 0u) return std::nullopt;
    const auto memoryIndex = word->Flags<MemoryFlags>().index;
    const auto& memories = program.Resources().memoryInfo;
    if (memoryIndex >= memories.size()) return std::nullopt;
    TableFormatColumn column;
    for (std::uint32_t slot = 0; slot < 4u; ++slot) {
        if (!FlatSlot(table->Argument(slot), column.tableSlots[slot])) return std::nullopt;
    }
    column.stride = stride->ImmediateU32();
    column.offset = memories[memoryIndex].offset;
    return column;
}

void CollectTableFormatColumns(IrProgram& program) {
    static const bool disabled = std::getenv("APS5_NO_TABLE_FORMATS") != nullptr;
    auto& columns = program.Resources().info.tableFormatColumns;
    columns.clear();
    if (disabled) return;
    const auto& memories = program.Resources().memoryInfo;
    for (const auto& block : program.Blocks()) {
        for (const IrValue* inst : block->Instructions()) {
            if (BufferAccessOf(inst->Opcode()) != BufferAccess::Read) continue;
            const auto memoryIndex = inst->Flags<MemoryFlags>().index;
            if (memoryIndex >= memories.size()) continue;
            const auto& memory = memories[memoryIndex];
            if (!memory.gpuDescriptor || !memory.formatted || memory.typed) continue;
            const auto column = TableFormatColumnOf(program, *inst);
            if (!column || std::find(columns.begin(), columns.end(), *column) != columns.end()) continue;
            if (columns.size() < PipelineSpecialization::MaxTableFormatColumns) columns.push_back(*column);
        }
    }
}

}
