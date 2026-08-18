#include "WorkflowReferenceSelectorProvider.h"

#include "SavorDbRuntime.h"
#include "UIRead/IUiReadDb.h"

#include <algorithm>
#include <tuple>

namespace savorqt::db {

ServiceResult<std::vector<WorkflowReferenceOption>> WorkflowReferenceSelectorProvider::List(
    const std::string& ref_kind,
    const std::string& data_kind,
    const std::string& search,
    int limit) {
    auto* db = savorqt::SavorDbRuntime::instance().uiReadDb();
    if (db == nullptr) return ServiceResult<std::vector<WorkflowReferenceOption>>::Err({ServiceErrorKind::Unavailable,kSavorDbRuntimeUnavailableMessage});
    std::vector<WorkflowReferenceOption> out;
    if (ref_kind == "state_artifact") {
        savor::db::UiReadArtifactListQuery query{}; query.limit=limit; query.search=search; query.extension=".dtm";
        for(const auto& row:db->ListArtifacts(query).items) if(row.artifact_kind=="DTM") out.push_back({row.artifact_id,"DTM #"+std::to_string(row.artifact_id)+" "+row.filename,row.sha256+" · "+std::to_string(row.size_bytes)+" bytes","COMPLETE"});
    } else if (ref_kind == "state.savestate") {
        std::string playback;
        if(data_kind.find("movie_inactive")!=std::string::npos)playback="MOVIE_INACTIVE";
        else if(data_kind.find("movie_paired")!=std::string::npos)playback="MOVIE_PAIRED";
        for(const auto& row:db->ListSavestates(playback,true,search,limit)) out.push_back({row.savestate_id,"State #"+std::to_string(row.savestate_id)+" "+row.filename,row.playback_state+" · "+row.sha256,row.is_complete?"COMPLETE":"INCOMPLETE"});
    } else if (ref_kind == "tmv_validation_attempt") {
        for(const auto& row:db->ListTasMovieValidationAttempts(std::nullopt,limit)) if(row.outcome=="ROOT_CURSOR_ESTABLISHED") out.push_back({row.validation_attempt_id,"Validation attempt #"+std::to_string(row.validation_attempt_id),"job "+std::to_string(row.source_job_id)+" · input "+std::to_string(row.actual_input_count),row.outcome});
    } else if (ref_kind == "state_tas_movie_tree") {
        for(const auto& row:db->ListTasMovieTrees(limit)) out.push_back({row.tas_movie_tree_id,"Recorded TAS branch #"+std::to_string(row.tas_movie_tree_id),"root "+std::to_string(row.tas_movie_root_id)+" · checkpoint "+std::to_string(row.checkpoint_savestate_id),"AVAILABLE"});
    } else if (ref_kind == "sp_probe_run") {
        savor::db::UiReadSeedProbeRunListQuery query{};query.limit=limit;query.search=search;query.only_completed=true;
        for(const auto& row:db->ListSeedProbeRuns(query).items) out.push_back({row.probe_run_id,"SeedProbe #"+std::to_string(row.probe_run_id),"state "+std::to_string(row.entry_savestate_id)+" · "+std::to_string(row.unique_count)+" unique",row.status});
    } else if (ref_kind == "ab_battle_context") {
        for(const auto& row:db->ListBattleContexts(true,limit)) out.push_back({row.context_probe_id,"Battle Context #"+std::to_string(row.context_probe_id),"state "+std::to_string(row.source_savestate_id),row.probe_status});
    } else {
        return ServiceResult<std::vector<WorkflowReferenceOption>>::Err({ServiceErrorKind::InvalidInput,"no typed selector provider for ref kind: "+ref_kind});
    }
    return ServiceResult<std::vector<WorkflowReferenceOption>>::Ok(std::move(out));
}

ServiceResult<std::vector<WorkflowReferenceOption>>
WorkflowReferenceSelectorProvider::ListPresentationFamily(
    const std::string& presentation_family_key,
    const std::string& search,
    int limit) {
    const auto registry =
        savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    std::vector<WorkflowReferenceOption> combined;
    bool found_family = false;
    for (const auto& unit : registry.ListUnits()) {
        if (unit.standalone_presentation_family_key !=
            presentation_family_key) continue;
        found_family = true;
        if (!unit.standalone_launchable || unit.hidden ||
            unit.required_inputs.size() != 1u) {
            return ServiceResult<std::vector<WorkflowReferenceOption>>::Err({
                ServiceErrorKind::InvalidInput,
                "standalone presentation family members require one visible typed input"});
        }
        const auto& input = unit.required_inputs.front();
        auto member = List(input.ref_kind, input.data_kind, search, limit);
        if (!member.ok) return member;
        for (auto& option : member.value) {
            option.member_unit_kind = unit.unit_kind;
            option.input_key = input.key;
            option.data_kind = input.data_kind;
            option.ref_kind = input.ref_kind;
            combined.push_back(std::move(option));
        }
    }
    if (!found_family) {
        return ServiceResult<std::vector<WorkflowReferenceOption>>::Err({
            ServiceErrorKind::NotFound,
            "standalone presentation family was not found: " +
                presentation_family_key});
    }
    std::sort(combined.begin(), combined.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.member_unit_kind, lhs.ref_id) <
            std::tie(rhs.member_unit_kind, rhs.ref_id);
    });
    return ServiceResult<std::vector<WorkflowReferenceOption>>::Ok(
        std::move(combined));
}

} // namespace savorqt::db
