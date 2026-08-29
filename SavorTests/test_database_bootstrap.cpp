#include <gtest/gtest.h>

#include "Common/DatabaseBootstrap.h"
#include "Common/DbService.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <ranges>
#include <string>

namespace {

class TemporaryBootstrapRoot final {
public:
    TemporaryBootstrapRoot()
    {
        root_ = std::filesystem::temp_directory_path()
            / ("savor-bootstrap-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root_);
    }

    ~TemporaryBootstrapRoot()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return root_; }

private:
    std::filesystem::path root_;
};

TEST(DatabaseBootstrap, StandardAndEmptyProfilesCreateExpectedRoots)
{
    TemporaryBootstrapRoot temporary;
    const auto database_root = temporary.path() / "db";
    std::filesystem::create_directories(database_root);
    savor::db::core::DBService service(
        savor::db::bootstrap::MakeDatabaseRootConfigPaths(database_root));
    std::string error;
    ASSERT_TRUE(service.Start(&error)) << error;

    const savor::db::bootstrap::DatabaseRootBootstrapService bootstrap;
    savor::db::bootstrap::DatabaseRootBootstrapResult standard{};
    ASSERT_TRUE(bootstrap.RecreateRoot(
        service,
        {database_root, savor::db::bootstrap::DatabaseBootstrapProfileId::Standard},
        &standard,
        &error)) << error;
    ASSERT_TRUE(standard.success);
    EXPECT_EQ(standard.created.seed_probe_specs, 1);
    EXPECT_EQ(standard.created.predicate_definitions, 1);
    EXPECT_EQ(standard.created.predicate_bindings, 1);
    EXPECT_EQ(standard.created.predicate_groups, 1);
    EXPECT_EQ(standard.created.battle_action_presets, 2);
    EXPECT_EQ(standard.created.battle_plans, 1);
    EXPECT_EQ(standard.created.workflow_graphs, 5);

    const auto specs = service.AuthoringDb()->ListSeedProbeSpecs(10);
    ASSERT_EQ(specs.size(), 1u);
    EXPECT_EQ(specs.front().name, "Basic");
    EXPECT_EQ(specs.front().min_value, 48);
    EXPECT_EQ(specs.front().max_value, 207);
    EXPECT_EQ(specs.front().combo_attempts_per_target, 32);
    EXPECT_EQ(specs.front().combo_sampler_tries, 8);

    const auto presets = service.AuthoringDb()->ListBattlePlanActionPresets(10);
    ASSERT_EQ(presets.size(), 2u);
    const auto groups = service.AuthoringDb()->ListPredicateGroupRevisions({
        .revision_state = std::string("PUBLISHED"), .limit = 10});
    ASSERT_EQ(groups.items.size(), 1u);
    EXPECT_EQ(groups.items.front().description,
        "Requires at least one Electribox drop per turn, evaluated at turn end and battle victory.");
    const auto plans = service.AuthoringDb()->ListBattlePlans(10);
    ASSERT_EQ(plans.size(), 1u);
    EXPECT_EQ(plans.front().name, "1st battle");
    EXPECT_EQ(plans.front().description,
        "Runs the two-turn first-battle strategy and evaluates the First battle predicate group on each turn.");
    ASSERT_EQ(plans.front().turns.size(), 2u);

    const auto graphs = service.AuthoringDb()->ListWorkflowGraphs(10, true);
    ASSERT_EQ(graphs.size(), 5u);
    const auto prepare_graph = std::ranges::find(
        graphs, std::string("TAS Prepare Root"),
        &savor::db::WorkflowGraphSnapshot::name);
    ASSERT_NE(prepare_graph, graphs.end());
    EXPECT_EQ(prepare_graph->nodes.size(), 3u);
    ASSERT_EQ(prepare_graph->edges.size(), 3u);
    EXPECT_EQ(std::ranges::count(prepare_graph->edges, std::string("DATA"),
        &savor::db::WorkflowGraphEdgeSnapshot::edge_kind), 2);
    EXPECT_EQ(std::ranges::count(prepare_graph->edges, std::string("CONTROL"),
        &savor::db::WorkflowGraphEdgeSnapshot::edge_kind), 1);
    const auto validate_node = std::ranges::find(
        prepare_graph->nodes, std::string("tas_movie_validate_root_2"),
        &savor::db::WorkflowGraphNodeSnapshot::node_key);
    ASSERT_NE(validate_node, prepare_graph->nodes.end());
    const auto rtc_argument = std::ranges::find(
        validate_node->arguments, std::string("rtc"),
        &savor::db::WorkflowGraphNodeArgumentSnapshot::argument_key);
    ASSERT_NE(rtc_argument, validate_node->arguments.end());
    EXPECT_EQ(rtc_argument->binding_mode,
        savor::db::SaveWorkflowGraphNodeArgumentCommand::BindingMode::Constant);
    EXPECT_EQ(rtc_argument->constant_value, "0");
    const auto battle_graph = std::ranges::find(
        graphs, std::string("First Battle Exploration"),
        &savor::db::WorkflowGraphSnapshot::name);
    ASSERT_NE(battle_graph, graphs.end());
    EXPECT_EQ(battle_graph->execution_shape, "EXPANSION");
    EXPECT_EQ(battle_graph->expansion_kind, "TAS_FIRST_BATTLE");
    EXPECT_EQ(battle_graph->nodes.size(), 6u);
    EXPECT_EQ(battle_graph->edges.size(), 6u);
    for (const auto& entry : std::filesystem::directory_iterator(temporary.path())) {
        const auto name = entry.path().filename().string();
        EXPECT_FALSE(name.starts_with("db.bootstrap-"));
        EXPECT_FALSE(name.starts_with("db.backup-"));
    }

    savor::db::bootstrap::DatabaseRootBootstrapResult empty{};
    ASSERT_TRUE(bootstrap.RecreateRoot(
        service,
        {database_root, savor::db::bootstrap::DatabaseBootstrapProfileId::Empty},
        &empty,
        &error)) << error;
    EXPECT_TRUE(service.AuthoringDb()->ListSeedProbeSpecs(10).empty());
    EXPECT_TRUE(service.AuthoringDb()->ListBattlePlans(10).empty());
    EXPECT_TRUE(service.AuthoringDb()->ListWorkflowGraphs(10, true).empty());
    service.Stop();
}

TEST(DatabaseBootstrap, ProfileFailureLeavesActiveRootUntouched)
{
    TemporaryBootstrapRoot temporary;
    const auto database_root = temporary.path() / "db";
    std::filesystem::create_directories(database_root);
    savor::db::core::DBService service(
        savor::db::bootstrap::MakeDatabaseRootConfigPaths(database_root));
    std::string error;
    ASSERT_TRUE(service.Start(&error)) << error;

    std::int64_t existing_id = 0;
    ASSERT_TRUE(service.AuthoringDb()->SaveSeedProbeSpec({
        .name = "existing",
        .priority = 0,
        .min_value = 1,
        .max_value = 1,
        .cap_trigger_top = true,
        .ignore_trigger_minmax = true,
        .combo_attempts_per_target = 1,
        .combo_sampler_tries = 1,
        .created_at_utc = savor::db::types::UtcNow(),
        .correlation_id = "bootstrap-test",
        .causation_id = "bootstrap-test",
    }, &existing_id, &error)) << error;

    const savor::db::bootstrap::DatabaseRootBootstrapService bootstrap;
    savor::db::bootstrap::DatabaseRootBootstrapResult result{};
    EXPECT_FALSE(bootstrap.RecreateRoot(
        service,
        {database_root,
         static_cast<savor::db::bootstrap::DatabaseBootstrapProfileId>(99)},
        &result,
        &error));
    EXPECT_TRUE(service.IsRunning());
    const auto specs = service.AuthoringDb()->ListSeedProbeSpecs(10);
    ASSERT_EQ(specs.size(), 1u);
    EXPECT_EQ(specs.front().seed_probe_spec_id, existing_id);
    EXPECT_EQ(specs.front().name, "existing");
    service.Stop();
}

} // namespace
