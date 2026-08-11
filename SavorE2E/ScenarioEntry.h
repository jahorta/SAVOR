#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "Cli.h"
#include "Execution/ProgramDB/TasMovieValidation/PreparedSterilizedCheckpointEvidence.h"

namespace savor::db::core {
class DBService;
}

namespace savor::e2e {

struct ResolvedE2eScenarioEntry {
    E2eScenarioEntrySource source =
        E2eScenarioEntrySource::ImportedSavestateFile;
    std::optional<std::int64_t> savestate_id;
    std::optional<savor::db::execution::programdb::tasmovieevidence::
        PreparedSterilizedCheckpointEvidence> prepared_checkpoint;
    std::string run_identity;
};

bool ResolveE2eScenarioEntry(
    const E2eScenarioDescriptor& descriptor,
    const CliOptions& options,
    savor::db::core::DBService* db_service,
    ResolvedE2eScenarioEntry* entry_out,
    std::string* error_out = nullptr);

bool RequalifyPreparedScenarioEntry(
    const ResolvedE2eScenarioEntry& entry,
    savor::db::core::DBService* db_service,
    std::string* error_out = nullptr);

} // namespace savor::e2e
