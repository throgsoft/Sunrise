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
    if (state::build_data::objective_definitions_ready()) return true;
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
            || rows.count > (blob.size() - rows.dataOffset) / tables::kObjectiveRowStride)
            return false;
        std::vector<state::build_data::objectives::Definition> definitions(
            static_cast<std::size_t>(rows.count));
        for (std::size_t i = 0; i < definitions.size(); ++i) {
            auto& definition = definitions[i];
            definition.definitionIndex = static_cast<std::uint16_t>(i);
            const auto at = rows.dataOffset + i * tables::kObjectiveRowStride;
            std::memcpy(
                &definition.definitionHash, blob.data() + at, sizeof definition.definitionHash);
            std::memcpy(&definition.completionValue,
                        blob.data() + at + tables::kObjectiveCompletionValueOffset,
                        sizeof definition.completionValue);
        }
        return state::build_data::publish_objective_definitions(definitions);
    } catch (...) {
        return false;
    }
}
} // namespace sunrise::client::content::items::packages
