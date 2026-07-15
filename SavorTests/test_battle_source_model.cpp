#include <BattlePredictionScenario.h>
#include <BattleSourceModel.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
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

TEST(SavorPredictBattleSourceModel, LoadsHashedCanonicalFirstBattleBundle) {
    const auto bundle = load_battle_source_bundle(
        "first-battle-soldiers-us-final");

    ASSERT_TRUE(bundle.ok) << (bundle.errors.empty() ? "" : bundle.errors.front());
    EXPECT_EQ(bundle.manifest.key, "first-battle-soldiers-us-final");
    EXPECT_EQ(bundle.manifest.encounter_identity, "alx-enemyevent-0");
    EXPECT_EQ(bundle.manifest.stage_identity, "sst-s001-record-0");
    EXPECT_FALSE(bundle.manifest_sha256.empty());
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

TEST(SavorPredictBattleSourceModel, ValidatesRosterAndSavestateFingerprint) {
    const auto bundle = load_battle_source_bundle(
        "first-battle-soldiers-us-final");
    ASSERT_TRUE(bundle.ok);
    const auto validation = validate_battle_source_bundle(
        bundle,
        first_battle_roster(),
        BattleSourceValidationInput{
            .savestate_sha256 = bundle.manifest.accepted_savestate_sha256.front(),
            .require_savestate_match = true,
        });
    EXPECT_TRUE(validation.ok);
    EXPECT_TRUE(validation.roster_matches);
    EXPECT_TRUE(validation.savestate_matches);

    auto wrong_roster = first_battle_roster();
    wrong_roster.slots_[5].id = 1;
    const auto mismatch = validate_battle_source_bundle(
        bundle,
        wrong_roster,
        BattleSourceValidationInput{
            .savestate_sha256 = std::string(64, '0'),
            .require_savestate_match = true,
        });
    EXPECT_FALSE(mismatch.ok);
    EXPECT_FALSE(mismatch.roster_matches);
    EXPECT_FALSE(mismatch.savestate_matches);
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
    EXPECT_EQ(scenario.source_manifest_key, "first-battle-soldiers-us-final");
    EXPECT_EQ(scenario.movement_backend,
              BattlePredictionMovementBackend::FrameStateMachine);
}

} // namespace
