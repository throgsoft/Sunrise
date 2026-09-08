#include "dawning_vendor_actions.h"

#include <limits>

#include "../../../core/logging/log.h"
#include "../../../core/runtime/wall_clock.h"
#include "../../../state/account/inventory/dawning_oven_state.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/build_data/vendors/vendor_catalog.h"
#include "../../../state/runtime/dawning_oven_delivery.h"
#include "../web_service_runtime.h"

namespace sunrise::server::web_service::vendor {
bool intercept_dawning_delivery(std::uint16_t opcode,
                                std::int32_t vendorIndex,
                                std::int32_t saleIndex,
                                std::uint16_t itemDefinitionIndex,
                                Outcome& outcome) noexcept {
    namespace catalog = state::build_data::vendors;
    namespace oven = state::runtime::detail::dawning;
    state::build_data::items::Definition offered{};
    if (!state::build_data::find_item_definition_index(itemDefinitionIndex, offered)) return false;
    catalog::IndexEntry entry{};
    catalog::Definition definition{};
    const bool vendorResolved =
        vendorIndex >= 0 && vendorIndex <= (std::numeric_limits<std::uint16_t>::max)()
        && catalog::find_index(static_cast<std::uint16_t>(vendorIndex), entry)
        && catalog::find(entry.definitionHash, definition)
        && definition.definitionHash == entry.definitionHash;
    bool delivery = false;
    bool recipientMatches = false;
    std::uint32_t cookieHash{};
    for (const auto& row : oven::kDeliveries) {
        if (row.offerHash != offered.definitionHash) continue;
        delivery = true;
        if (vendorResolved && row.vendorHash == entry.definitionHash) {
            recipientMatches = true;
            cookieHash = row.cookieHash;
        }
    }
    // A sale explicitly charging a cookie is an exchange even when its dialog was not retained.
    catalog::SaleRow sale{};
    state::build_data::items::Definition cost{};
    if (vendorResolved && saleIndex >= 0
        && catalog::sale_row(definition, static_cast<std::size_t>(saleIndex), sale)
        && sale.itemIndex == itemDefinitionIndex
        && state::build_data::find_item_definition_index(sale.costItemIndex, cost)
        && state::account::inventory::dawning::cookie(cost.definitionHash)) {
        delivery = true;
        recipientMatches =
            recipientMatches && cookieHash == cost.definitionHash && sale.costQuantity == 1;
    } else if (delivery && saleIndex >= 0
               && (sale.itemIndex != itemDefinitionIndex
                   || sale.costItemIndex != catalog::kAbsentCostItem)) {
        recipientMatches = false;
    }
    if (!delivery) return false;
    auto* mutation = emplace_mutation<state::PendingRecordRewardGrant>(outcome);
    const bool staged = recipientMatches && mutation
                        && oven::stage_delivery(state::account_snapshot(),
                                                entry.definitionHash,
                                                offered.definitionHash,
                                                core::runtime::investment_clock_seconds(),
                                                *mutation);
    if (!staged) clear_mutation(outcome);
    // No vendor answer is claimed: Give Gift remains repeatable while cookies remain. Normal
    // 901/904 offers fall through without changing their installed-row resolution.
    core::log::writef(core::log::Channel::server,
                      staged ? core::log::Level::info : core::log::Level::warn,
                      "ev=dawning_delivery opcode=%u vendor=%d sale=%d item=%u cookie=0x%08X "
                      "result=%s policy=SunriseWorldGearAnd100Glimmer cookie_debit_staged=%u",
                      static_cast<unsigned>(opcode),
                      vendorIndex,
                      saleIndex,
                      static_cast<unsigned>(itemDefinitionIndex),
                      cookieHash,
                      staged ? "prepared" : "refused",
                      staged ? 1U : 0U);
    return true;
}
} // namespace sunrise::server::web_service::vendor
