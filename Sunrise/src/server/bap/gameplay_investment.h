#pragma once
#include "../../middleware/bap/activity_message/incident.h"
#include "internal.h"

namespace sunrise::server::bap {
/** Called under the BAP session lock after authenticated incident framing.
 * Independent of the bounded Host diagnostic and mission-input queues.
 */
void invest_gameplay_incident_locked(
    const ActivityClientBinding& binding,
    const middleware::bap::activity_message::incident::Incident& incident) noexcept;
} // namespace sunrise::server::bap
