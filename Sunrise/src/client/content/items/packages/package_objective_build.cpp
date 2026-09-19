#include "package_objective_build.h"

#include <cstring>
#include <vector>

#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../state/build_data/runtime.h"

namespace sunrise::client::content::items::packages {
namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;

bool build_objectives(const reader::Source& source,
                      reader::Scratch& scratch,
                      std::span<const std::byte> root) noexcept {
    if (state::build_data::objective_definitions_ready()) {
        return true;
    }
    try {
        std::uint32_t tag = 0;
        std::vector<std::byte> blob;
        tables::Array rows{};
        if (!tables::slot_tag(root, tables::kObjectiveTableSlot, tag)
            || tables::package_of(tag) == tables::kAbsentPackageId
            || !reader::read_tag(source, scratch, tag, blob)
            || !tables::find_array_at(blob, tables::kTableArrayDescriptor, rows)
            || rows.elementClass != tables::kObjectiveRowClass || rows.count == 0
            || rows.count > state::build_data::objectives::kDefinitionCapacity
            || rows.dataOffset > blob.size()
            || rows.count > (blob.size() - rows.dataOffset) / tables::kObjectiveRowStride) {
            return false;
        }
        std::vector<state::build_data::objectives::Definition> definitions(
            static_cast<std::size_t>(rows.count));
        // Numeric source definitions distinguish item counters from shared/unused values.
        // An objective reference alone does not prove that its ordinal addresses an item lane.
        std::vector<std::byte> sources;
        tables::Array sourceRows{};
        constexpr std::size_t sourceTableSlot = 114;
        constexpr std::uint32_t sourceRowClass = 0x80807C96U;
        constexpr std::size_t sourceStride = 8;
        if (!tables::slot_tag(root, sourceTableSlot, tag)
            || !reader::read_tag(source, scratch, tag, sources)
            || !tables::find_array_at(sources, tables::kTableArrayDescriptor, sourceRows)
            || sourceRows.elementClass != sourceRowClass || sourceRows.count == 0
            || sourceRows.dataOffset > sources.size()
            || sourceRows.count > (sources.size() - sourceRows.dataOffset) / sourceStride) {
            return false;
        }
        for (std::size_t i = 0; i < definitions.size(); ++i) {
            auto& definition = definitions[i];
            definition.definitionIndex = static_cast<std::uint16_t>(i);
            const auto at = rows.dataOffset + i * tables::kObjectiveRowStride;
            std::memcpy(
                &definition.definitionHash, blob.data() + at, sizeof definition.definitionHash);
            std::memcpy(&definition.completionValue,
                        blob.data() + at + tables::kObjectiveCompletionValueOffset,
                        sizeof definition.completionValue);
            tables::Array expression{};
            if (!tables::find_optional_array_at(
                    blob, at + tables::kObjectiveSourceExpressionField, expression)) {
                return false;
            }
            if (expression.count != 1
                || expression.elementClass != tables::kInvestmentExpressionRowClass
                || expression.dataOffset > blob.size() || blob.size() - expression.dataOffset < 8) {
                continue;
            }
            std::uint32_t opcode{}, slot{};
            std::memcpy(&opcode, blob.data() + expression.dataOffset, sizeof opcode);
            std::memcpy(&slot, blob.data() + expression.dataOffset + 4, sizeof slot);
            if (opcode != tables::kUnlockReadValueOpcode || slot >= sourceRows.count) {
                continue;
            }
            const auto sourceAt = sourceRows.dataOffset + slot * sourceStride;
            const auto kind = std::to_integer<std::uint8_t>(sources[sourceAt + 4]);
            const auto flags = std::to_integer<std::uint8_t>(sources[sourceAt + 5]);
            definition.itemProgress = kind == 4 && (flags & 0xFBU) == 0;
        }
        return state::build_data::publish_objective_definitions(definitions);
    } catch (...) {
        return false;
    }
}
} // namespace sunrise::client::content::items::packages
