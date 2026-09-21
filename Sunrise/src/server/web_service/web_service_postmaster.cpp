#include "web_service_actions.h"
#include "../../core/logging/log.h"
#include "../../middleware/web_service/messages/opcode405.h"
#include "../../state/runtime/postmaster_runtime.h"

namespace sunrise::server::web_service {
void claim_postmaster_item(const middleware::web_service::Message& message,
                           Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode405::Request request{};
    if (!middleware::web_service::messages::opcode405::parse_request(message, request)
        || request.vendorIndex < 0 || request.sourceBucket < 0 || request.definitionIndex < 0
        || request.quantity <= 0 || request.instanceSoid == 0) {
        core::log::write(core::log::Channel::server, core::log::Level::warn,
                         "ev=postmaster stage=request result=refused");
        return;
    }
    auto* mutation = emplace_mutation<state::PendingPostmasterClaim>(outcome);
    const bool prepared = mutation && state::prepare_postmaster_claim(
        static_cast<std::uint16_t>(request.vendorIndex),
        static_cast<std::uint8_t>(request.sourceBucket), request.instanceSoid,
        static_cast<std::uint16_t>(request.definitionIndex), request.quantity, *mutation);
    if (!prepared) clear_mutation(outcome);
    core::log::writef(core::log::Channel::server,
                      prepared ? core::log::Level::info : core::log::Level::warn,
                      "ev=postmaster stage=prepare result=%s vendor=%d bucket=%d item=%d "
                      "instance=0x%llX quantity=%d",
                      prepared ? "ready" : "refused", request.vendorIndex, request.sourceBucket,
                      request.definitionIndex, static_cast<unsigned long long>(request.instanceSoid),
                      request.quantity);
}
}
