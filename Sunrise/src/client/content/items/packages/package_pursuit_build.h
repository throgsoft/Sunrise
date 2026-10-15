#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../../../../middleware/content/packages/reader/reader.h"
#include "../../../../middleware/content/packages/tables/items.h"
#include "../../../../state/build_data/pursuits/definition.h"
#include "package_reward_build.h"

namespace sunrise::client::content::items::packages {

/** Logs a refusal the installed data decides and settles the objective and pursuit domains. */
void settle_pursuits_unsupported(const char* reason) noexcept;

/**
 * Loads the condition tables objective and pursuit programs bind through.
 * Tables the reward domain already refused settle the pursuit domains as well.
 * @return True when the tables loaded; false leaves a transient read for the next pass.
 */
[[nodiscard]] bool
load_pursuit_conditions(RewardConditions& conditions,
                        const middleware::content::packages::reader::Source& source,
                        middleware::content::packages::reader::Scratch& scratch,
                        std::span<const std::byte> root,
                        const SlotMaps& maps) noexcept;

/** Collects pursuit completion contracts during the item walk, including warm-cache startup. */
class PursuitBuild {
public:
    /** Loads the condition tables and probes the completion operation table. */
    [[nodiscard]] bool begin(const middleware::content::packages::reader::Source& source,
                             middleware::content::packages::reader::Scratch& scratch,
                             std::span<const std::byte> root,
                             const SlotMaps& maps) noexcept;
    /** Collects one item's contract; false leaves that item without one. */
    [[nodiscard]] bool item(const middleware::content::packages::tables::items::Row& row) noexcept;
    /** Counts an item whose definition could not be read; it simply has no contract. */
    void unread(std::uint16_t index) noexcept;
    /** Publishes the collected contracts, settling the domain when State refuses them. */
    void publish() noexcept;

private:
    [[nodiscard]] state::build_data::pursuits::ValueReference
    value(std::uint16_t index) const noexcept;
    [[nodiscard]] state::build_data::pursuits::OperationReference
    operation(std::uint16_t index) const noexcept;
    RewardConditions conditions_;
    std::vector<state::build_data::pursuits::Definition> rows_;
    std::uint64_t operationCount_{};
    std::size_t unreadCount_{};
    std::uint16_t firstUnread_{state::build_data::pursuits::kAbsent};
    bool operationTablePresent_{};
    bool ready_{};
};

} // namespace sunrise::client::content::items::packages
