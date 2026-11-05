#ifndef CORE_SHADER_RECOMPILER_OPTIMIZATION_TABLEFORMATS_HPP
#define CORE_SHADER_RECOMPILER_OPTIMIZATION_TABLEFORMATS_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include <optional>

namespace ShaderRecompiler {

// The record-table column a run-time formatted buffer load takes its format word from, when its V# is read at
// record * stride from a table whose own V# lies in flat SRT slots.
[[nodiscard]] std::optional<TableFormatColumn> TableFormatColumnOf(const IrProgram& program, const IrValue& load);
// Fills ShaderInfo::tableFormatColumns. APS5_NO_TABLE_FORMATS=1 leaves it empty: every load keeps the run-time decoder.
void CollectTableFormatColumns(IrProgram& program);

}

#endif
