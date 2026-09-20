#include <algorithm>
#include <cstring>
#include <limits>

#include "internal.h"
#include "package_reward_build.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace domain = state::build_data::season_pass;

/** Account progression the installed Season of Arrivals pass declares its rewards on. */
constexpr std::uint16_t kPassProgressionIndex = 40;

/** @param blob Source bytes. @param offset Field offset. @param value Receives the field. */
template <typename Value>
[[nodiscard]] bool
read(std::span<const std::byte> blob, std::size_t offset, Value& value) noexcept {
    if (offset > blob.size() || blob.size() - offset < sizeof value) {
        return false;
    }
    std::memcpy(&value, blob.data() + offset, sizeof value);
    return true;
}

} // namespace

/**
 * Reads the season pass reward list and fixed socket overrides.
 * A reward names an item index and a claim flag slot; both become the values a grant needs.
 * @param source Installed package source.
 * @param storage Pass storage receiving the reward rows.
 * @param root Investment root bytes.
 * @return True when the reward list read and produced at least one row.
 */
bool build_season_pass(const reader::Source& source,
                       Storage& storage,
                       std::span<const std::byte> root) noexcept {
    storage.seasonPassRewardCount = 0;
    RewardConditions conditions;
    if (!conditions.load(source, storage.scratch, root)) return false;

    std::uint32_t itemTableTag = 0;
    tables::Array itemRows{};
    if (!tables::slot_tag(root, tables::kItemTableSlot, itemTableTag) || itemTableTag == 0
        || !reader::read_tag(source, storage.scratch, itemTableTag, storage.itemIndexTable)
        || !tables::find_array_at(std::span<const std::byte>{storage.itemIndexTable},
                                  tables::kTableArrayDescriptor,
                                  itemRows)
        || itemRows.elementClass != tables::kItemIndexTableClass) {
        return false;
    }
    const std::span<const std::byte> itemTable{storage.itemIndexTable};

    std::uint32_t tableTag = 0;
    tables::Array progressions{};
    if (!tables::slot_tag(root, tables::kProgressionTableSlot, tableTag) || tableTag == 0
        || !reader::read_tag(source, storage.scratch, tableTag, storage.progressionTable)
        || !tables::find_array_at(std::span<const std::byte>{storage.progressionTable},
                                  tables::kTableArrayDescriptor,
                                  progressions)
        || progressions.elementClass != tables::kProgressionTableClass
        || progressions.count <= kPassProgressionIndex) {
        return false;
    }
    const std::span<const std::byte> progressionTable{storage.progressionTable};
    const std::size_t passAt =
        progressions.dataOffset + kPassProgressionIndex * tables::kProgressionRowStride;
    tables::Array rewards{};
    if (!tables::find_optional_array_at(
            progressionTable, passAt + tables::kProgressionRewardField, rewards)
        || rewards.count == 0 || rewards.elementClass != tables::kProgressionRewardRowClass
        || rewards.count > domain::kRewardCapacity
        || rewards.dataOffset
                   + static_cast<std::size_t>(rewards.count) * tables::kProgressionRewardStride
               > progressionTable.size()) {
        return false;
    }

    for (std::uint64_t row = 0; row < rewards.count; ++row) {
        const std::size_t at =
            rewards.dataOffset + static_cast<std::size_t>(row) * tables::kProgressionRewardStride;
        std::uint32_t rank = 0;
        std::uint32_t itemIndex = 0;
        std::uint32_t claimSlot = 0;
        domain::Reward& reward = storage.seasonPassRewards[storage.seasonPassRewardCount];
        reward = {};
        tables::IndexRow entry{};
        if (!read(progressionTable, at + tables::kProgressionRewardRankOffset, rank)
            || !read(progressionTable, at + tables::kProgressionRewardItemIndexOffset, itemIndex)
            || !read(
                progressionTable, at + tables::kProgressionRewardQuantityOffset, reward.quantity)
            || !read(progressionTable, at + tables::kProgressionRewardClaimSlotOffset, claimSlot)
            || rank > (std::numeric_limits<std::uint8_t>::max)()
            || itemIndex > (std::numeric_limits<std::uint16_t>::max)()
            || claimSlot > (std::numeric_limits<std::uint16_t>::max)()
            || !tables::index_row(itemTable, itemRows, itemIndex, entry)) {
            storage.seasonPassRewardCount = 0;
            return false;
        }
        reward.itemHash = entry.definitionHash;
        reward.itemIndex = static_cast<std::uint16_t>(itemIndex);
        reward.requiredRank = static_cast<std::uint8_t>(rank);
        std::size_t socketCount = 0;
        // Progression reward rows append their socket overrides at byte 40.
        if (!read_reward_sockets(progressionTable, at + 40, reward.sockets, socketCount)) {
            return false;
        }
        reward.socketCount = static_cast<std::uint8_t>(socketCount);
        std::size_t conditionCount = 0;
        if (!conditions.read_list(progressionTable, at + 24, reward.condition, conditionCount))
            return false;
        reward.conditionCount = static_cast<std::uint8_t>(conditionCount);
        // A reward with no claim flag carries slot 0.
        if (claimSlot != 0) {
            reward.claimFlagIndex =
                bank_index(storage.slotMaps.accountFlag, static_cast<std::int32_t>(claimSlot));
        }

        ++storage.seasonPassRewardCount;
    }
    return storage.seasonPassRewardCount != 0;
}

} // namespace sunrise::client::content::items::packages
