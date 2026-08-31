#include <gtest/gtest.h>

#include "Execution/Workflow/WorkflowExpansionService.h"
#include "Execution/ProgramDB/TasMovieValidation/TasMovieInputEpochProgram.h"

namespace {
using savor::db::execution::workflow::NormalizeExactFirstBattleExpansionTargets;
using savor::db::execution::workflow::WorkflowExpansionTarget;
using savor::db::execution::programdb::tasmovieinputepoch::HashTasMovieDelayPreparationIdentity;
using savor::db::execution::programdb::tasmovieinputepoch::TasMovieDelayPreparationIdentity;

TEST(WorkflowExpansionTargets, PreservesExactRequestedCoordinates) {
    const auto targets = NormalizeExactFirstBattleExpansionTargets({
        {.neutral_epoch_count=5, .rtc_value=355},
        {.neutral_epoch_count=0, .rtc_value=355},
        {.neutral_epoch_count=1, .rtc_value=356},
    });
    const std::vector<WorkflowExpansionTarget> expected{
        {0,355}, {1,356}, {5,355},
    };
    EXPECT_EQ(targets, expected);
}

TEST(WorkflowExpansionTargets, DeduplicatesAndRejectsInvalidCoordinates) {
    const auto targets = NormalizeExactFirstBattleExpansionTargets({
        {.neutral_epoch_count=1, .rtc_value=9},
        {.neutral_epoch_count=1, .rtc_value=9},
        {.neutral_epoch_count=-1, .rtc_value=9},
        {.neutral_epoch_count=2, .rtc_value=-1},
    });
    const std::vector<WorkflowExpansionTarget> expected{{1,9}};
    EXPECT_EQ(targets, expected);
}

TEST(TasMovieDelayPreparationIdentity, IsStableForTheSameSemantics) {
    TasMovieDelayPreparationIdentity identity{
        .source_dtm_sha256=std::string(64, '1'),
        .source_schedule_sha256=std::string(64, '2'),
        .root_pc=0x80100000u,
        .root_movie_input_cursor=400,
        .root_itinerary_sha256=std::string(64, '3'),
        .insert_before_epoch=350,
        .neutral_epoch_count=4,
        .placement_profile="first_battle.final_dialog",
        .full_phase_program_kind=14,
        .full_phase_program_version=1,
        .full_phase_canonical_id="soa.tasmovie.revise",
        .full_phase_contract_revision=1,
        .full_phase_sha256=std::string(64, '4'),
        .module_canonical_id="soa.tasmovie.revise.module",
        .module_revision=1,
        .module_sha256=std::string(64, '5'),
        .revise_graph_revision_id=9,
    };
    const auto first = HashTasMovieDelayPreparationIdentity(identity);
    EXPECT_EQ(first, HashTasMovieDelayPreparationIdentity(identity));
    EXPECT_EQ(first.size(), 64u);
}

TEST(TasMovieDelayPreparationIdentity, SemanticDifferencesDoNotReuse) {
    TasMovieDelayPreparationIdentity identity{
        .source_dtm_sha256=std::string(64, '1'),
        .source_schedule_sha256=std::string(64, '2'),
        .root_pc=1,
        .root_movie_input_cursor=2,
        .root_itinerary_sha256=std::string(64, '3'),
        .insert_before_epoch=3,
        .neutral_epoch_count=4,
        .placement_profile="first_battle.final_dialog",
        .full_phase_program_kind=14,
        .full_phase_program_version=1,
        .full_phase_canonical_id="phase",
        .full_phase_contract_revision=1,
        .full_phase_sha256=std::string(64, '4'),
        .module_canonical_id="module",
        .module_revision=1,
        .module_sha256=std::string(64, '5'),
        .revise_graph_revision_id=6,
    };
    const auto original = HashTasMovieDelayPreparationIdentity(identity);
    ++identity.neutral_epoch_count;
    EXPECT_NE(original, HashTasMovieDelayPreparationIdentity(identity));
    --identity.neutral_epoch_count;
    ++identity.revise_graph_revision_id;
    EXPECT_NE(original, HashTasMovieDelayPreparationIdentity(identity));
    --identity.revise_graph_revision_id;
    identity.source_schedule_sha256 = std::string(64, '9');
    EXPECT_NE(original, HashTasMovieDelayPreparationIdentity(identity));
}
}
