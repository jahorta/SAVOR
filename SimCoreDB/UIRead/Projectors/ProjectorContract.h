#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "../../Common/Events/OutboxRelay.h"

namespace simcore::db::uiread::projectors {

bool ValidateProjectorContractInputs(
    const std::string& projector_name,
    int max_batch_size,
    int max_attempts,
    std::string* error_out);

std::int64_t GetProjectorCheckpoint(
    sqlite3* db,
    const std::string& projector_name,
    std::string* error_out);

bool RunProjectorRelay(
    sqlite3* db,
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table,
    const events::OutboxRelayConfig& relay_config,
    const std::vector<events::OutboxRelayDispatchBinding>& bindings,
    int max_batch_size,
    std::string* error_out);

bool RunProjectorRelay(
    sqlite3* db,
    const std::string& checkpoint_name,
    const events::OutboxRelayConfig& relay_config,
    const std::vector<events::OutboxRelayDispatchBinding>& bindings,
    int max_batch_size,
    std::string* error_out);

} // namespace simcore::db::uiread::projectors
