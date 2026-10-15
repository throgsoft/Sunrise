#include "studio_investment_link.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../core/settings/settings.h"
#include "../../middleware/studio/studio_investment_wire.h"
#include "../../middleware/studio/studio_mapping.h"
#include "../../state/account/inventory/inventory_edit.h"
#include "../bap/investment_operations.h"
#include "studio_command_protocol.h"
#include "studio_investment_payloads.h"

namespace sunrise::server::studio {
namespace {

namespace wire = middleware::studio::investment_wire;

/** Each opcode's State owner, operation lane and incremental publication exist in this build. */
constexpr std::uint64_t kAdvertisedCapabilities =
    wire::capability(wire::Opcode::hello) | wire::capability(wire::Opcode::inventorySnapshot)
    | wire::capability(wire::Opcode::seasonPassSnapshot)
    | wire::capability(wire::Opcode::pursuitSnapshot) | wire::capability(wire::Opcode::grantItem)
    | wire::capability(wire::Opcode::setQuantity)
    | wire::capability(wire::Opcode::setSeasonalExperience)
    | wire::capability(wire::Opcode::claimSeasonPassReward)
    | wire::capability(wire::Opcode::resetSeasonPassClaim)
    | wire::capability(wire::Opcode::readReceipt)
    | wire::capability(wire::Opcode::acknowledgeReceipt)
    | wire::capability(wire::Opcode::withdrawOperation);

HANDLE g_mapping{};
wire::SharedInvestment* g_view{};
std::uint64_t g_nextConnect{};
/** The process instance this link advertised when it connected. */
std::uint64_t g_gameStarted{};

/** Operations and receipts outlive the link, so an editor that returns can still read them. */
void disconnect() noexcept {
    if (g_view != nullptr) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         "ev=studio stage=investment result=disconnected");
    }
    middleware::studio::close_editor_view(g_mapping, g_view);
}

/** @return The process creation FILETIME, which tells a recycled process id apart; zero on failure.
 */
[[nodiscard]] std::uint64_t process_started() noexcept {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == FALSE) {
        return 0;
    }
    ULARGE_INTEGER started{};
    started.LowPart = creation.dwLowDateTime;
    started.HighPart = creation.dwHighDateTime;
    return started.QuadPart;
}

/** @return True only when an editor created a mapping of this exact protocol. */
[[nodiscard]] bool connect() noexcept {
    // Every version keeps this control block, so it is mapped alone before the size is known.
    wire::Control* control =
        middleware::studio::open_editor_view<wire::Control>(wire::kMappingFormat, g_mapping);
    if (control == nullptr) {
        return false;
    }
    // Studio reads gameVersion to explain a version mismatch.
    if (control->magic == wire::kMagic) {
        control->gamePid = GetCurrentProcessId();
        control->gameVersion = wire::kVersion;
    }
    const bool accepted = middleware::studio::editor_header_ok(
        *control, wire::kMagic, wire::kVersion, wire::kMappingBytes);
    UnmapViewOfFile(control);
    const std::uint64_t started = accepted ? process_started() : 0;
    if (started != 0) {
        g_view = middleware::studio::map_editor_view<wire::SharedInvestment>(g_mapping);
    }
    if (g_view == nullptr) {
        disconnect();
        return false;
    }
    g_gameStarted = started;
    g_view->control.gameStarted = started;
    g_view->control.capabilities = kAdvertisedCapabilities;
    core::log::write(core::log::Channel::server,
                     core::log::Level::info,
                     "ev=studio stage=investment result=connected");
    return true;
}

/** @return One of the two ids, read with a full barrier. */
[[nodiscard]] std::uint32_t read_id(volatile std::int32_t& id) noexcept {
    return static_cast<std::uint32_t>(
        InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&id), 0, 0));
}

/** @return An invalid-request answer with one static label and no context or payload. */
[[nodiscard]] InvestmentAnswer invalid(const char* reason) noexcept {
    return {.status = wire::Status::invalid, .reason = reason};
}

/** A mutation names every part of the context it was read against. */
[[nodiscard]] bool context_complete(const wire::Context& context) noexcept {
    return context.accountSoid != 0 && context.characterSoid != 0 && context.contentHash != 0;
}

/** Requests other than mutations bind to no context. */
[[nodiscard]] bool context_empty(const wire::Context& context) noexcept {
    return context.accountSoid == 0 && context.characterSoid == 0 && context.contentHash == 0;
}

/**
 * Checks the shape every request of an advertised opcode shares.
 * @return Null when the block can be served, else the label of an invalid request.
 */
[[nodiscard]] const char* malformed(const wire::Request& request) noexcept {
    if (request.payloadBytes != wire::request_bytes(request.opcode)) {
        return "request_size";
    }
    const auto unused = std::span(request.payload).subspan(request.payloadBytes);
    if (std::any_of(
            unused.begin(), unused.end(), [](std::byte value) { return value != std::byte{}; })) {
        return "request_padding";
    }
    const bool identified =
        wire::is_mutation(request.opcode) || wire::is_receipt_request(request.opcode);
    if ((request.operationId != 0) != identified) {
        return "request_operation";
    }
    if (wire::is_mutation(request.opcode) ? !context_complete(request.expected)
                                          : !context_empty(request.expected)) {
        return "request_context";
    }
    return nullptr;
}

/** Copies one typed request out of the copied payload bytes. */
template <typename Payload>
[[nodiscard]] Payload payload_of(const wire::Request& request) noexcept {
    static_assert(sizeof(Payload) <= wire::kRequestPayloadBytes);
    Payload payload{};
    std::memcpy(&payload, request.payload.data(), sizeof(Payload));
    return payload;
}

/** @return False for a scope value the wire does not define. */
[[nodiscard]] bool edit_scope(wire::InventoryScope scope,
                              state::account::inventory::EditScope& output) noexcept {
    switch (scope) {
    case wire::InventoryScope::profile:
        output = state::account::inventory::EditScope::profile;
        return true;
    case wire::InventoryScope::characterItem:
        output = state::account::inventory::EditScope::characterItem;
        return true;
    case wire::InventoryScope::characterStack:
        output = state::account::inventory::EditScope::characterStack;
        return true;
    }
    return false;
}

/** @return False when a reserved or enumerated field holds a value the opcode never sends. */
[[nodiscard]] bool decode_operation(const wire::Request& request,
                                    bap::InvestmentOperation& operation) noexcept {
    operation.id = request.operationId;
    operation.target = {
        request.expected.accountSoid, request.expected.characterSoid, request.expected.contentHash};
    switch (request.opcode) {
    case wire::Opcode::grantItem: {
        const auto grant = payload_of<wire::GrantItemRequest>(request);
        operation.request = bap::GrantItemOperation{grant.itemHash, grant.quantity};
        return true;
    }
    case wire::Opcode::setQuantity: {
        const auto edit = payload_of<wire::SetQuantityRequest>(request);
        state::account::inventory::EditScope scope{};
        if (!edit_scope(edit.scope, scope) || edit.reserved != decltype(edit.reserved){}) {
            return false;
        }
        operation.request = state::account::inventory::QuantityEdit{scope,
                                                                    edit.characterSoid,
                                                                    edit.instanceSoid,
                                                                    edit.definitionHash,
                                                                    edit.expectedSerial,
                                                                    edit.expectedQuantity,
                                                                    edit.quantity};
        return true;
    }
    case wire::Opcode::setSeasonalExperience: {
        const auto experience = payload_of<wire::SetSeasonalExperienceRequest>(request);
        operation.request =
            bap::SeasonalExperienceOperation{experience.expected, experience.replacement};
        return true;
    }
    case wire::Opcode::claimSeasonPassReward: {
        const auto claim = payload_of<wire::ClaimSeasonPassRewardRequest>(request);
        if (claim.reserved != 0) {
            return false;
        }
        operation.request = bap::SeasonPassClaimOperation{claim.rowIndex, claim.itemHash};
        return true;
    }
    case wire::Opcode::resetSeasonPassClaim: {
        const auto reset = payload_of<wire::ResetSeasonPassClaimRequest>(request);
        if (reset.expectedClaimed != 1 || reset.reserved != 0) {
            return false;
        }
        operation.request = bap::SeasonPassResetOperation{reset.rowIndex, reset.itemHash};
        return true;
    }
    default:
        return false;
    }
}

/** A receipt answers every retained submission, also one State refused before it was queued. */
[[nodiscard]] InvestmentAnswer submit(const bap::InvestmentOperation& operation,
                                      InvestmentPayload payload) noexcept {
    bap::InvestmentReceipt receipt{};
    const bap::InvestmentSubmission submission =
        bap::submit_investment_operation(operation, receipt);
    InvestmentAnswer answer = encode_receipt(operation.id, receipt, payload);
    switch (submission) {
    case bap::InvestmentSubmission::accepted:
    case bap::InvestmentSubmission::existing:
        break;
    case bap::InvestmentSubmission::reused:
        answer.status = wire::Status::refused;
        answer.reason = "operation_reused";
        break;
    case bap::InvestmentSubmission::refused:
        // Nothing was retained, so the status says so; the receipt names why.
        answer.status = wire::Status::refused;
        break;
    }
    return answer;
}

/** An unknown id leaves the receipt empty, which answers unknown. */
[[nodiscard]] InvestmentAnswer read_receipt(std::uint64_t operationId,
                                            InvestmentPayload payload) noexcept {
    bap::InvestmentReceipt receipt{};
    static_cast<void>(bap::investment_receipt(operationId, receipt));
    return encode_receipt(operationId, receipt, payload);
}

/** A pending receipt stays retained and is refused with the receipt it still holds. */
[[nodiscard]] InvestmentAnswer acknowledge_receipt(std::uint64_t operationId,
                                                   InvestmentPayload payload) noexcept {
    bap::InvestmentReceipt receipt{};
    const bool forgotten = bap::acknowledge_investment_receipt(operationId, receipt);
    InvestmentAnswer answer = encode_receipt(operationId, receipt, payload);
    if (!forgotten && receipt.operationId != 0) {
        answer.status = wire::Status::refused;
        answer.reason = "receipt_pending";
    }
    return answer;
}

/** The editor reads the receipt's state to learn whether the withdrawal came too late. */
[[nodiscard]] InvestmentAnswer withdraw_operation(std::uint64_t operationId,
                                                  InvestmentPayload payload) noexcept {
    bap::InvestmentReceipt receipt{};
    static_cast<void>(bap::withdraw_investment_operation(operationId, receipt));
    return encode_receipt(operationId, receipt, payload);
}

/** Answers one copied request; nothing it does reads the shared request block again. */
[[nodiscard]] InvestmentAnswer serve(const wire::Request& request) noexcept {
    if (request.opcode == wire::Opcode::none || request.opcode >= wire::Opcode::count) {
        return invalid("request_opcode");
    }
    if ((kAdvertisedCapabilities & wire::capability(request.opcode)) == 0) {
        return {.status = wire::Status::unsupported, .reason = "request_unsupported"};
    }
    if (const char* reason = malformed(request)) {
        return invalid(reason);
    }
    const InvestmentPayload payload(g_view->response.payload);
    switch (request.opcode) {
    case wire::Opcode::hello:
        return encode_hello(g_gameStarted, kAdvertisedCapabilities, payload);
    case wire::Opcode::inventorySnapshot:
        return encode_inventory_snapshot(payload);
    case wire::Opcode::seasonPassSnapshot:
        return encode_season_pass_snapshot(payload);
    case wire::Opcode::pursuitSnapshot:
        return encode_pursuit_snapshot(payload);
    case wire::Opcode::readReceipt:
        return read_receipt(request.operationId, payload);
    case wire::Opcode::acknowledgeReceipt:
        return acknowledge_receipt(request.operationId, payload);
    case wire::Opcode::withdrawOperation:
        return withdraw_operation(request.operationId, payload);
    case wire::Opcode::grantItem:
    case wire::Opcode::setQuantity:
    case wire::Opcode::setSeasonalExperience:
    case wire::Opcode::claimSeasonPassReward:
    case wire::Opcode::resetSeasonPassClaim: {
        bap::InvestmentOperation operation{};
        if (!decode_operation(request, operation)) {
            return invalid("request_field");
        }
        return submit(operation, payload);
    }
    case wire::Opcode::none:
    case wire::Opcode::count:
        break;
    }
    return invalid("request_opcode");
}

/**
 * Open views repeat hello, snapshot and receipt requests every second, so a served one is debug;
 * mutations, withdrawals and every answer other than ok stay at info.
 */
[[nodiscard]] core::log::Level answer_level(wire::Opcode opcode, wire::Status status) noexcept {
    const bool read = !wire::is_mutation(opcode) && opcode != wire::Opcode::withdrawOperation;
    return read && status == wire::Status::ok ? core::log::Level::debug : core::log::Level::info;
}

/** Writes the response block, then the response id, so Studio never reads half an answer. */
void answer(std::uint32_t request,
            const wire::Request& copy,
            const InvestmentAnswer& result) noexcept {
    wire::Response& response = g_view->response;
    response.status = result.status;
    response.reserved = 0;
    response.operationId = copy.operationId;
    response.opcode = copy.opcode;
    response.payloadBytes = result.payloadBytes;
    response.context = result.context;
    response.reason = {};
    const std::string_view reason(result.reason != nullptr ? result.reason : "");
    std::copy_n(
        reason.data(), (std::min)(reason.size(), wire::kReasonBytes - 1), response.reason.data());
    MemoryBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_view->control.response),
                        static_cast<LONG>(request));
    core::log::writef(core::log::Channel::server,
                      answer_level(copy.opcode, result.status),
                      "ev=studio stage=investment result=answered request=%u opcode=%u status=%u "
                      "op=%llu reason=%s",
                      request,
                      static_cast<unsigned>(copy.opcode),
                      static_cast<unsigned>(result.status),
                      static_cast<unsigned long long>(copy.operationId),
                      reason.empty() ? "none" : reason.data());
}

} // namespace

/** Each request id gets exactly one answer, in the slice that copies it. */
void service_investment(std::uint64_t now) noexcept {
    // Off by default: no mapping is ever opened.
    if (!core::settings::get().studioAttach) {
        if (g_view != nullptr) {
            disconnect();
        }
        return;
    }
    if (g_view == nullptr && now >= g_nextConnect) {
        g_nextConnect = now + command_wire::kConnectIntervalMs;
        static_cast<void>(connect());
    }
    if (g_view == nullptr) {
        return;
    }
    if (!middleware::studio::editor_alive(
            g_view->control.heartbeat, now, command_wire::kHeartbeatTimeoutMs)) {
        disconnect();
        return;
    }
    const std::uint32_t request = read_id(g_view->control.request);
    if (request == 0 || request == read_id(g_view->control.response)) {
        return;
    }
    MemoryBarrier();
    const wire::Request copy = g_view->request;
    MemoryBarrier();
    // Studio replaced the request while it was copied; the next slice serves the new one.
    if (read_id(g_view->control.request) != request) {
        return;
    }
    answer(request, copy, serve(copy));
}

/** Runs after the server thread stops, so no slice still uses the view. */
void shutdown_investment() noexcept {
    disconnect();
    g_nextConnect = 0;
    g_gameStarted = 0;
}

} // namespace sunrise::server::studio
