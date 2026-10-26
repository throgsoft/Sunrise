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
 * Loads the condition tables that objective and pursuit programs bind through. When the reward
 * domain has already settled these tables as unsupported, the pursuit domains settle too.
 * @return True when the tables loaded. A transient read failure retries on the next pass.
 */
[[nodiscard]] bool
load_pursuit_conditions(RewardConditions& conditions,
                        const middleware::content::packages::reader::Source& source,
                        middleware::content::packages::reader::Scratch& scratch,
                        std::span<const std::byte> root,
                        const SlotMaps& maps) noexcept;

/** Collects pursuit completion contracts during the item walk that fills the cache. */
class PursuitBuild {
public:
    /** Loads the condition tables and probes the completion operation table. */
    [[nodiscard]] bool begin(const middleware::content::packages::reader::Source& source,
                             middleware::content::packages::reader::Scratch& scratch,
                             std::span<const std::byte> root,
                             const SlotMaps& maps) noexcept;
    /** Collects one item's contract; false when storing it failed, which a later walk retries. */
    [[nodiscard]] bool item(const middleware::content::packages::tables::items::Row& row) noexcept;
    /** Counts an item the installed data leaves unreadable. That item gets no contract. */
    void unread(std::uint16_t index) noexcept;
    /**
     * Notes a cataloged item whose read or storage failed, which says nothing about the data.
     * Publication waits for a walk without one.
     */
    void read_failed(std::uint16_t index) noexcept;
    /**
     * Publishes the collected contracts beside the item catalog, pruning those for items it lacks.
     * Contracts that still fail validation settle the domain.
     */
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
    std::size_t readFailures_{};
    std::uint16_t firstReadFailure_{state::build_data::pursuits::kAbsent};
    bool operationTablePresent_{};
    bool ready_{};
};

} // namespace sunrise::client::content::items::packages
