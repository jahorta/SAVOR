#include <BattlePredictionScenario.h>
#include <BattleSourceModel.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>

namespace {

using namespace savor::predict;

soa::battle::ctx::BattleContext first_battle_roster() {
    soa::battle::ctx::BattleContext context;
    for (const auto [slot, is_player, id] : {
             std::tuple{0, true, 0},
             std::tuple{1, true, 1},
             std::tuple{4, false, 0},
             std::tuple{5, false, 0}}) {
        context.slots_[slot].present = 1;
        context.slots_[slot].is_player = is_player ? 1 : 0;
        context.slots_[slot].id = static_cast<std::uint16_t>(id);
        context.slots_[slot].is_alive = 1;
    }
    return context;
}

BattleSourceSelection first_battle_source_selection() {
    return first_battle_soldiers_prediction_scenario().source_selection;
}

TEST(SavorPredictBattleSourceModel, LoadsHashedCanonicalFirstBattleBundle) {
    const auto bundle = load_battle_source_bundle(
        "first-battle-soldiers-us-final");

    ASSERT_TRUE(bundle.ok) << (bundle.errors.empty() ? "" : bundle.errors.front());
    EXPECT_EQ(bundle.manifest.key, "first-battle-soldiers-us-final");
    EXPECT_EQ(bundle.manifest.encounter_identity, "alx-enemyevent-0");
    EXPECT_EQ(bundle.manifest.stage_identity, "sst-s001-record-0");
    EXPECT_EQ(bundle.manifest.producer_kind,
              BattleSourceProducerKind::ScriptedBattleRequest);
    ASSERT_TRUE(bundle.manifest.scripted_battle_request.has_value());
    const auto& request = *bundle.manifest.scripted_battle_request;
    EXPECT_EQ(request.identity.script_identity, "me201a.sct");
    EXPECT_EQ(request.identity.section_identity, "loop");
    EXPECT_EQ(request.identity.instruction_payload_offset, 396);
    EXPECT_EQ(request.instruction_offset, 244);
    EXPECT_EQ(request.event_mode, 1);
    EXPECT_EQ(request.event_or_encounter_id, 0);
    EXPECT_EQ(request.stage_id, 1);
    EXPECT_EQ(request.transition_selector, 3);
    EXPECT_EQ(request.status, BattleSourceFieldStatus::Exact);
    EXPECT_FALSE(bundle.manifest_sha256.empty());
    EXPECT_EQ(bundle.snapshot.encounter_source_kind,
              BattleEncounterSourceKind::EventDefinition);
    EXPECT_EQ(bundle.snapshot.requested_encounter_id, 0);
    EXPECT_EQ(bundle.snapshot.encounter_id, 0);
    EXPECT_EQ(bundle.snapshot.stage_id, "s001");
    EXPECT_EQ(bundle.snapshot.terrain_status, BattleSourceFieldStatus::Exact);
    EXPECT_EQ(bundle.snapshot.terrain_source_9x9[0], 1u);
    EXPECT_EQ(bundle.snapshot.terrain_source_9x9[2 * 9 + 2], 0u);
    EXPECT_EQ(bundle.snapshot.placements.size(), 4u);
    const auto slot5 = battle_source_placement_for_slot(bundle.snapshot, 5);
    ASSERT_TRUE(slot5.has_value());
    EXPECT_EQ(slot5->grid_x, 6);
}

TEST(SavorPredictBattleSourceModel, ResolvesFirstBattleByScriptedRequestIdentity) {
    const auto resolved = resolve_battle_source_bundle(
        first_battle_source_selection());

    ASSERT_EQ(resolved.status, BattleSourceResolutionStatus::Exact);
    ASSERT_TRUE(resolved.bundle.ok);
    EXPECT_EQ(resolved.bundle.manifest.key, "first-battle-soldiers-us-final");
    EXPECT_EQ(resolved.bundle.snapshot.stage_id, "s001");
    EXPECT_EQ(resolved.bundle.snapshot.sst_record_index, 0);
    EXPECT_EQ(resolved.candidate_manifest_keys.size(), 1u);
    EXPECT_FALSE(resolved.provenance.empty());
}

TEST(SavorPredictBattleSourceModel, RejectsScriptStageThatDisagreesWithSnapshot) {
    const auto source = load_battle_source_bundle(
        "first-battle-soldiers-us-final");
    ASSERT_TRUE(source.ok);
    const auto root = std::filesystem::temp_directory_path()
        / "savor-scripted-battle-stage-mismatch";
    const auto bundle_dir = root / "first-battle-soldiers-us-final";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(bundle_dir);
    std::filesystem::copy_file(
        source.snapshot_path,
        bundle_dir / "snapshot.json",
        std::filesystem::copy_options::overwrite_existing);

    std::ifstream input(source.manifest_path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    auto manifest = text.str();
    const std::string before = "\"stageId\": 1";
    const auto stage = manifest.find(before);
    ASSERT_NE(stage, std::string::npos);
    manifest.replace(stage, before.size(), "\"stageId\": 2");
    std::ofstream output(bundle_dir / "manifest.json", std::ios::binary);
    output << manifest;
    output.close();

    const auto invalid = load_battle_source_bundle(
        "first-battle-soldiers-us-final",
        root);
    EXPECT_FALSE(invalid.ok);
    ASSERT_FALSE(invalid.errors.empty());
    EXPECT_NE(invalid.errors.front().find("operands do not match"),
              std::string::npos);
    std::filesystem::remove_all(root);
}

TEST(SavorPredictBattleSourceModel, ReportsMissingAndUnsupportedSourceProducers) {
    const auto missing = resolve_battle_source_bundle({
        .producer_kind = BattleSourceProducerKind::ScriptedBattleRequest,
        .scripted_request = ScriptedBattleRequestIdentity{
            .script_identity = "missing.sct",
            .section_identity = "loop",
            .instruction_payload_offset = 396,
        },
    });
    EXPECT_EQ(missing.status, BattleSourceResolutionStatus::MissingInput);

    const auto unsupported = resolve_battle_source_bundle({
        .producer_kind = BattleSourceProducerKind::RandomEncounterTable,
    });
    EXPECT_EQ(unsupported.status, BattleSourceResolutionStatus::Unsupported);

    const auto incomplete = resolve_battle_source_bundle({
        .producer_kind = BattleSourceProducerKind::ScriptedBattleRequest,
    });
    EXPECT_EQ(incomplete.status, BattleSourceResolutionStatus::MissingInput);
}

TEST(SavorPredictBattleSourceModel, ValidatesEncounterAndTreatsSavestateAsProvenance) {
    const auto bundle = load_battle_source_bundle(
        "first-battle-soldiers-us-final");
    ASSERT_TRUE(bundle.ok);
    const BattleEncounterIdentity event0{
        .source_kind = BattleEncounterSourceKind::EventDefinition,
        .encounter_id = 0,
    };
    const auto validation = validate_battle_source_bundle(
        bundle,
        first_battle_roster(),
        BattleSourceValidationInput{
            .source_selection = first_battle_source_selection(),
            .expected_encounter = event0,
            .savestate_sha256 = bundle.manifest.accepted_savestate_sha256.front(),
        });
    EXPECT_TRUE(validation.ok);
    EXPECT_TRUE(validation.roster_matches);
    EXPECT_TRUE(validation.source_selection_matches);
    EXPECT_TRUE(validation.encounter_matches);
    EXPECT_TRUE(validation.savestate_fingerprint_recognized);

    auto wrong_roster = first_battle_roster();
    wrong_roster.slots_[5].id = 1;
    const auto mismatch = validate_battle_source_bundle(
        bundle,
        wrong_roster,
        BattleSourceValidationInput{
            .source_selection = first_battle_source_selection(),
            .expected_encounter = BattleEncounterIdentity{
                .source_kind = BattleEncounterSourceKind::EventDefinition,
                .encounter_id = 1,
            },
            .savestate_sha256 = std::string(64, '0'),
        });
    EXPECT_FALSE(mismatch.ok);
    EXPECT_FALSE(mismatch.roster_matches);
    EXPECT_FALSE(mismatch.encounter_matches);
    EXPECT_FALSE(mismatch.savestate_fingerprint_recognized);

    const auto unrecognized_provenance = validate_battle_source_bundle(
        bundle,
        first_battle_roster(),
        BattleSourceValidationInput{
            .source_selection = first_battle_source_selection(),
            .expected_encounter = event0,
            .savestate_sha256 = std::string(64, '0'),
        });
    EXPECT_TRUE(unrecognized_provenance.ok);
    EXPECT_TRUE(unrecognized_provenance.roster_matches);
    EXPECT_TRUE(unrecognized_provenance.encounter_matches);
    EXPECT_FALSE(unrecognized_provenance.savestate_fingerprint_recognized);
    EXPECT_FALSE(unrecognized_provenance.diagnostics.empty());

    auto wrong_source = first_battle_source_selection();
    wrong_source.scripted_request.instruction_payload_offset = 397;
    const auto source_mismatch = validate_battle_source_bundle(
        bundle,
        first_battle_roster(),
        BattleSourceValidationInput{
            .source_selection = wrong_source,
        });
    EXPECT_FALSE(source_mismatch.ok);
    EXPECT_FALSE(source_mismatch.source_selection_matches);
}

TEST(SavorPredictBattleSourceModel, RecognizesPreviouslyObservedSavestateFingerprints) {
    const auto bundle = load_battle_source_bundle(
        "first-battle-soldiers-us-final");
    ASSERT_TRUE(bundle.ok);

    for (const auto hash : {
             "83bf5d205d9e77d92d47996bf731715de4440ce4bb6475623432a58df77fcf42",
             "b73e1aff52e1abb1fc113b62c304d84f703ec36e0d00084df97f8da8278f12cc",
             "af59e7828407e06e65c403bcbb28bba26f9d66ff5b1ce0ed1bcd16bb7426e4b6",
             "6a49d9dce2929995031b6d67245d01a6779675ba6198f1f68ff81bbfa0c40261",
             "9247a369a45d6d3e135133bc2234431483ef9fc245fab9cef12d2565cda08d8c"}) {
        const auto validation = validate_battle_source_bundle(
            bundle,
            first_battle_roster(),
            BattleSourceValidationInput{
                .source_selection = first_battle_source_selection(),
                .expected_encounter = BattleEncounterIdentity{
                    .source_kind = BattleEncounterSourceKind::EventDefinition,
                    .encounter_id = 0,
                },
                .savestate_sha256 = hash,
            });
        EXPECT_TRUE(validation.ok) << hash;
        EXPECT_TRUE(validation.savestate_fingerprint_recognized) << hash;
    }
}

TEST(SavorPredictBattleSourceModel, MissingManifestDoesNotBackfillState) {
    const auto bundle = load_battle_source_bundle(
        "missing-manifest",
        std::filesystem::temp_directory_path() / "savor-no-battle-sources");
    EXPECT_FALSE(bundle.ok);
    EXPECT_FALSE(bundle.errors.empty());
}

TEST(SavorPredictBattleSourceModel, DerivesStdIdentityFromSideAndId) {
    const auto pc = derive_battle_std_resource_identity(0, true, 0);
    const auto enemy = derive_battle_std_resource_identity(4, false, 0);
    const auto giant = derive_battle_std_resource_identity(4, false, 0x80);
    const auto unsupported = derive_battle_std_resource_identity(4, false, 0x9e);

    EXPECT_EQ(pc.status, BattleStdResourceIdentityStatus::Exact);
    EXPECT_EQ(pc.stem, "MA000");
    EXPECT_EQ(enemy.stem, "MB000");
    EXPECT_EQ(giant.stem, "MG000");
    EXPECT_EQ(unsupported.status, BattleStdResourceIdentityStatus::MissingInput);
}

TEST(SavorPredictBattleSourceModel, ScenarioCarriesOnlySourceAndOperationalDefaults) {
    const auto scenario = first_battle_soldiers_prediction_scenario();
    EXPECT_EQ(scenario.name, "first-battle-soldiers");
    EXPECT_EQ(scenario.profile_name, "first-battle-soldiers");
    EXPECT_EQ(scenario.source_selection.producer_kind,
              BattleSourceProducerKind::ScriptedBattleRequest);
    EXPECT_EQ(scenario.source_selection.scripted_request.script_identity,
              "me201a.sct");
    EXPECT_EQ(scenario.source_selection.scripted_request.section_identity,
              "loop");
    EXPECT_EQ(
        scenario.source_selection.scripted_request.instruction_payload_offset,
        396);
}

} // namespace
