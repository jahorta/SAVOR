#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "../../Common/Events/OutboxRelay.h"

namespace simcore::db::uiread::projectors {

class UiOutboxRelayCoordinator {
public:
    explicit UiOutboxRelayCoordinator(sqlite3* db);

    bool RelayAll(
        const std::string& projector_name,
        int max_batch_size,
        std::string* error_out,
        int max_attempts = 5,
        bool include_archive = false);

    bool RelayExecutionOutbox(const std::string& projector_name, int max_batch_size, std::string* error_out, int max_attempts = 5);
    bool RelayStateOutbox(const std::string& projector_name, int max_batch_size, std::string* error_out, int max_attempts = 5);
    bool RelaySeedProbeOutbox(const std::string& projector_name, int max_batch_size, std::string* error_out, int max_attempts = 5);
    bool RelayAnalysisBattleOutbox(const std::string& projector_name, int max_batch_size, std::string* error_out, int max_attempts = 5);
    bool RelayAnalysisSpineOutbox(const std::string& projector_name, int max_batch_size, std::string* error_out, int max_attempts = 5);
    bool RelayAuthoringOutbox(const std::string& projector_name, int max_batch_size, std::string* error_out, int max_attempts = 5);
    bool RelayArchiveOutbox(const std::string& projector_name, int max_batch_size, std::string* error_out, int max_attempts = 5);

private:
    bool ValidateInputs(const std::string& projector_name, int max_batch_size, std::string* error_out, int max_attempts) const;
    std::int64_t GetCheckpoint(const std::string& projector_name) const;
    bool UpsertCheckpoint(const std::string& projector_name, std::int64_t last_outbox_id, std::string* error_out) const;
    bool RelayWithConfig(
        const std::string& checkpoint_name,
        const events::OutboxRelayConfig& config,
        const std::vector<events::OutboxRelayDispatchBinding>& bindings,
        int max_batch_size,
        std::string* error_out) const;

    sqlite3* db_ = nullptr;
};

} // namespace simcore::db::uiread::projectors
