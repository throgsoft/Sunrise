#include "../../core/logging/log.h"
#include "../../middleware/web_service/messages/opcode2002.h"
#include "../../state/runtime/bright_engram_runtime.h"
#include "web_service_actions.h"

namespace sunrise::server::web_service {
void redeem_bright_engram(const middleware::web_service::Message& message,
                          Outcome& outcome) noexcept {
    namespace codec = middleware::web_service::messages::opcode2002;
    codec::Request request{};
    const bool parsed = codec::parse_request(message, request);
    if (!parsed || request.instanceSoid == 0) {
        core::log::writef(
            core::log::Channel::server,
            core::log::Level::warn,
            "ev=ws2002 stage=request result=refused parsed=%u bytes=%zu",
            parsed ? 1U : 0U,
            message.payload.size());
        return;
    }
    auto* mutation = emplace_mutation<state::PendingRecordRewardGrant>(outcome);
    if (mutation == nullptr
        || !state::bright_engrams::prepare_redemption(
            request.instanceSoid, -1, *mutation)) {
        clear_mutation(outcome);
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws2002 stage=prepare result=refused");
        return;
    }
    core::log::writef(core::log::Channel::server,
                      core::log::Level::info,
                      "ev=ws2002 stage=prepare result=ok source=%016llX policy=%s",
                      static_cast<unsigned long long>(request.instanceSoid),
                      state::build_data::eververse::kEngramPolicy);
}
} // namespace sunrise::server::web_service
