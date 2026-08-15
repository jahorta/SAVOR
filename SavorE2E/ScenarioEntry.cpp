#include "ScenarioEntry.h"

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

#include "Common/DbService.h"
#include "DbSetup.h"

namespace savor::e2e {
namespace {

std::string NewRunIdentity(std::string_view scenario) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto ticks = std::chrono::system_clock::now()
        .time_since_epoch().count();
    return std::string(scenario) + "-" + std::to_string(ticks) + "-"
        + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

bool Fail(std::string message, std::string* error_out) {
    if (error_out != nullptr) *error_out = std::move(message);
    return false;
}

} // namespace

bool ResolveE2eScenarioEntry(
    const E2eScenarioDescriptor& descriptor,
    const CliOptions& options,
    savor::db::core::DBService* db_service,
    ResolvedE2eScenarioEntry* entry_out,
    std::string* error_out) {
    if (db_service == nullptr || !db_service->IsRunning()
        || db_service->StateDb() == nullptr
        || db_service->AnalysisDb() == nullptr || entry_out == nullptr) {
        return Fail("scenario entry resolver requires running State and Analysis databases",
                    error_out);
    }

    ResolvedE2eScenarioEntry entry{};
    entry.source = SelectE2eScenarioEntrySource(descriptor, options);
    entry.run_identity = NewRunIdentity(descriptor.name);
    switch (entry.source) {
    case E2eScenarioEntrySource::ImportedSavestateFile: {
        std::int64_t savestate_id = 0;
        if (!SeedStateSavestate(
                db_service->StateDb(), options.savestate_file,
                &savestate_id, error_out)) {
            return false;
        }
        entry.savestate_id = savestate_id;
        break;
    }
    case E2eScenarioEntrySource::FreshTasMovieValidation:
        break;
    case E2eScenarioEntrySource::ExistingWorkspaceReference:
        break;
    case E2eScenarioEntrySource::PreparedSterilizedCheckpoint: {
        if (!options.source_savestate_id
            || *options.source_savestate_id <= 0) {
            return Fail("prepared checkpoint entry lacks a positive source savestate id",
                        error_out);
        }
        savor::db::execution::programdb::tasmovieevidence::
            PreparedSterilizedCheckpointEvidence evidence{};
        if (!savor::db::execution::programdb::tasmovieevidence::
                ResolvePreparedSterilizedCheckpointEvidence(
                    db_service->StateDb(), db_service->AnalysisDb(),
                    *options.source_savestate_id, &evidence, error_out)) {
            return false;
        }
        entry.savestate_id = *options.source_savestate_id;
        entry.prepared_checkpoint = std::move(evidence);
        break;
    }
    }
    *entry_out = std::move(entry);
    return true;
}

bool RequalifyPreparedScenarioEntry(
    const ResolvedE2eScenarioEntry& entry,
    savor::db::core::DBService* db_service,
    std::string* error_out) {
    if (entry.source
        != E2eScenarioEntrySource::PreparedSterilizedCheckpoint) {
        return true;
    }
    if (!entry.savestate_id || !entry.prepared_checkpoint
        || db_service == nullptr || db_service->StateDb() == nullptr
        || db_service->AnalysisDb() == nullptr) {
        return Fail("prepared checkpoint entry cannot be requalified", error_out);
    }
    savor::db::execution::programdb::tasmovieevidence::
        PreparedSterilizedCheckpointEvidence current{};
    if (!savor::db::execution::programdb::tasmovieevidence::
            ResolvePreparedSterilizedCheckpointEvidence(
                db_service->StateDb(), db_service->AnalysisDb(),
                *entry.savestate_id, &current, error_out)) {
        return false;
    }
    const auto& expected = *entry.prepared_checkpoint;
    if (current.selected_checkpoint.savestate_id
            != expected.selected_checkpoint.savestate_id
        || current.selected_checkpoint.artifact_id
            != expected.selected_checkpoint.artifact_id
        || current.selected_checkpoint.artifact_sha256
            != expected.selected_checkpoint.artifact_sha256
        || current.sterilization_derivation.derivation_id
            != expected.sterilization_derivation.derivation_id
        || current.sterilization_request.sterilization_request_id
            != expected.sterilization_request.sterilization_request_id
        || current.sterilization_attempt.sterilization_attempt_id
            != expected.sterilization_attempt.sterilization_attempt_id
        || current.validation_attempt.validation_attempt_id
            != expected.validation_attempt.validation_attempt_id) {
        return Fail("prepared checkpoint identity changed during the scenario",
                    error_out);
    }
    return true;
}

} // namespace savor::e2e
