#include "eververse_vendor_actions.h"

#include "../../../core/logging/log.h"
#include "../../../middleware/web_service/messages/opcode402.h"
#include "../../../middleware/web_service/messages/opcode905.h"
#include "../../../state/build_data/eververse/manifest_catalog.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/runtime/eververse_runtime.h"
#include "../web_service_runtime.h"

namespace sunrise::server::web_service::vendor {
bool intercept_cosmetic_unlock(
    const middleware::web_service::messages::opcode402::Request& request,
    Outcome& outcome) noexcept {
    state::build_data::items::Definition item{};
    state::build_data::eververse::ItemMetadata metadata{};
    if (request.definitionIndex < 0
        || !state::build_data::find_item_definition_index(
            static_cast<std::uint16_t>(request.definitionIndex), item)
        || !state::build_data::eververse::read_item(item.definitionHash, metadata))
        return false;
    if (metadata.isWrapper) {
        const char* reason = "package_source_requires_instance";
        bool prepared = false;
        if (request.hasInstance && request.instanceSoid != 0) {
            auto* mutation = emplace_mutation<state::PendingRecordRewardGrant>(outcome);
            reason = "mutation_storage_unavailable";
            prepared = mutation
                && state::eververse::prepare_package_action(
                    false, request.instanceSoid, item.definitionIndex, request.value,
                    request.selector, false, 0, *mutation, reason);
        }
        if (!prepared) clear_mutation(outcome);
        core::log::writef(core::log::Channel::server,
                          prepared ? core::log::Level::info : core::log::Level::warn,
                          "ev=package_open opcode=402 item=%u selector=%d result=%s reason=%s",
                          static_cast<unsigned>(item.definitionIndex),
                          static_cast<int>(request.selector),
                          prepared ? "prepared" : "refused", reason);
        return true;
    }
    if (!metadata.unlockAction) return false;
    auto* mutation = emplace_mutation<state::PendingRecordRewardGrant>(outcome);
    const char* reason = "mutation_storage_unavailable";
    const bool prepared = mutation
        && state::eververse::prepare_unlock(request.hasInstance, request.instanceSoid,
                                            item.definitionIndex, request.value, request.selector,
                                            *mutation, reason);
    if (!prepared) clear_mutation(outcome);
    core::log::writef(core::log::Channel::server,
                      prepared ? core::log::Level::info : core::log::Level::warn,
                      "ev=cosmetic_unlock opcode=402 item=%u selector=%d result=%s reason=%s",
                      static_cast<unsigned>(item.definitionIndex),
                      static_cast<int>(request.selector), prepared ? "prepared" : "refused", reason);
    return true;
}

void refund_package(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    namespace codec = middleware::web_service::messages::opcode905;
    codec::Request request{};
    const char* reason = "invalid_refund_request";
    bool prepared = false;
    if (codec::parse_request(message, request)) {
        reason = "unsupported_refund_source";
        if (request.mode == 1 && request.instanceSoid != 0 && request.definitionIndex >= 0) {
            auto* mutation = emplace_mutation<state::PendingRecordRewardGrant>(outcome);
            reason = "mutation_storage_unavailable";
            prepared = mutation
                && state::eververse::prepare_package_action(
                    true, request.instanceSoid, static_cast<std::uint16_t>(request.definitionIndex),
                    1, -1, request.hasClock, request.clock, *mutation, reason);
        }
    }
    if (!prepared) clear_mutation(outcome);
    core::log::writef(core::log::Channel::server,
                      prepared ? core::log::Level::info : core::log::Level::warn,
                      "ev=package_refund opcode=%u mode=%d item=%d result=%s reason=%s",
                      static_cast<unsigned>(message.opcode), static_cast<int>(request.mode),
                      static_cast<int>(request.definitionIndex),
                      prepared ? "prepared" : "refused", reason);
}

bool intercept_eververse_purchase(std::uint16_t opcode,
                                  std::int32_t vendorIndex,
                                  std::int32_t saleIndex,
                                  std::uint16_t itemDefinitionIndex,
                                  Outcome& outcome) noexcept {
    if (!state::eververse::is_store_vendor(vendorIndex)) return false;
    auto* mutation = emplace_mutation<state::PendingRecordRewardGrant>(outcome);
    const char* reason = "mutation_storage_unavailable";
    if (opcode != 901) reason = "unsupported_store_purchase_opcode";
    const bool prepared = opcode == 901
        && mutation && state::eververse::prepare_purchase(vendorIndex, saleIndex, *mutation, reason);
    if (!prepared) clear_mutation(outcome);
    core::log::writef(
        core::log::Channel::server,
        prepared ? core::log::Level::info : core::log::Level::warn,
        "ev=eververse_purchase opcode=%u vendor=%d sale=%d item=%u result=%s reason=%s",
        static_cast<unsigned>(opcode),
        vendorIndex,
        saleIndex,
        static_cast<unsigned>(itemDefinitionIndex),
        prepared ? "prepared" : "refused",
        reason);
    return true;
}
} // namespace sunrise::server::web_service::vendor
