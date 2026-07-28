#include <gtest/gtest.h>

#include <BattlePredictionBatchCli.h>
#include <BattlePredictionBatchRun.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace savor::predict;

class TempTree final {
public:
    explicit TempTree(std::string_view label)
        : path_(
            std::filesystem::temp_directory_path()
            / (std::string(label)
                + std::to_string(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count()))) {
        std::filesystem::create_directories(path_);
    }

    ~TempTree() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

bool contains_error(
    const std::vector<std::string>& errors,
    std::string_view expected) {
    return std::any_of(
        errors.begin(),
        errors.end(),
        [&](const std::string& error) {
            return error.find(expected) != std::string::npos;
        });
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

BattlePredictorResourceBundlePtr ready_resource_inputs(
    BattlePredictorResourceProviderKind provider =
        BattlePredictorResourceProviderKind::DirectSpice) {
    auto bundle = std::make_shared<BattlePredictorResourceBundle>();
    bundle->status = BattlePredictorResourceInputStatus::Ready;
    bundle->provider_kind = provider;
    bundle->adapter_version = "test-adapter-v1";
    bundle->spice_revision = "test-spice-revision";
    bundle->bundle_digest = "test-resource-digest";
    bundle->sources.push_back({
        .logical_role = "test.primary_std",
        .normalized_relative_path = "bchara/test.std",
        .size_bytes = 4,
        .sha256 = "test-source-sha256",
        .parser_identity = "test-parser",
        .parser_status = "ready",
    });
    return bundle;
}

TEST(
    SavorPredictBattlePredictionBatchRun,
    OptionValidationReportsInvalidJobAndFrontDoorConfiguration) {
    TempTree tree("savor_predict_batch_validation_");
    const auto missing_db_root = tree.path() / "missing-db";
    const auto missing_std_json_dir = tree.path() / "missing-std-json";

    BattlePredictionBatchRunOptions options;
    options.source_exec_job_ids = {42, 42, 0, -7};
    options.db_root = missing_db_root;
    options.profile_name = "not-a-prediction-profile";
    options.action_view_std_json_dir = missing_std_json_dir;
    options.run_root = tree.path() / "run";

    const auto errors =
        validate_battle_prediction_batch_run_options(options);

    EXPECT_TRUE(contains_error(errors, "Duplicate source exec-job ID: 42"));
    EXPECT_TRUE(contains_error(errors, "must be positive"));
    EXPECT_TRUE(contains_error(errors, "DB root is not an existing directory"));
    EXPECT_TRUE(
        contains_error(errors, "Unsupported prediction profile"));
    EXPECT_TRUE(contains_error(errors, "batch run name is required"));
}

TEST(
    SavorPredictBattlePredictionBatchRun,
    CliCombinesRepeatedAndListJobIdsUnderTheDefaultRunRoot) {
    TempTree tree("savor_predict_batch_cli_");
    const auto list_path = tree.path() / "jobs.txt";
    {
        std::ofstream list(list_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(list);
        list << "# canonical controls\n"
             << "202, 303\n";
    }

    const auto parsed = parse_battle_prediction_batch_tokens(
        {
            "--exec-job-id",
            "101",
            "--exec-job-list",
            list_path.string(),
            "--run-name",
            "canonical-seven",
            "--preflight-only",
        },
        tree.path() / "SavorPredict.exe");

    EXPECT_TRUE(parsed.errors.empty())
        << (parsed.errors.empty() ? std::string{} : parsed.errors.front());
    EXPECT_EQ(
        parsed.options.batch.source_exec_job_ids,
        (std::vector<long long>{101, 202, 303}));
    EXPECT_EQ(
        parsed.options.batch.run_root.generic_string(),
        ".codex-runs/predict/canonical-seven");
    EXPECT_EQ(
        parsed.options.batch.db_root.generic_string(),
        "D:/SavorPredictDB");
    EXPECT_EQ(
        parsed.options.batch.scenario_name.value_or(""),
        "first-battle-soldiers");
    EXPECT_TRUE(parsed.options.batch.preflight_only);
}

TEST(
    SavorPredictBattlePredictionBatchRun,
    CliRejectsRunNamePathTraversal) {
    const auto parsed = parse_battle_prediction_batch_tokens(
        {
            "--exec-job-id",
            "101",
            "--run-name",
            "../outside",
        },
        "SavorPredict.exe");

    EXPECT_TRUE(contains_error(
        parsed.errors,
        "--run-name must be a single path-safe name"));
}

TEST(
    SavorPredictBattlePredictionBatchRun,
    CliReconcilesDeprecatedDiscDumpRootAlias) {
    const auto equivalent = parse_battle_prediction_batch_tokens(
        {
            "--exec-job-id", "101",
            "--disc-dump-root", "D:/disc",
            "--std-disc-dump-root", "D:/disc/.",
            "--spice-file-parsing-exe", "D:/tools/SpiceFileParsing.exe",
        },
        "SavorPredict.exe");

    EXPECT_TRUE(equivalent.errors.empty())
        << (equivalent.errors.empty()
            ? std::string{}
            : equivalent.errors.front());
    EXPECT_EQ(
        equivalent.options.disc_dump_root,
        std::filesystem::path("D:/disc"));
    EXPECT_NE(
        std::find_if(
            equivalent.warnings.begin(),
            equivalent.warnings.end(),
            [](const std::string& warning) {
                return warning.find("--spice-file-parsing-exe")
                    != std::string::npos
                    && warning.find("ignored") != std::string::npos;
            }),
        equivalent.warnings.end());

    const auto conflicting = parse_battle_prediction_batch_tokens(
        {
            "--exec-job-id", "101",
            "--disc-dump-root", "D:/disc",
            "--std-disc-dump-root", "D:/other",
        },
        "SavorPredict.exe");
    EXPECT_TRUE(contains_error(
        conflicting.errors,
        "must resolve to the same path"));
}

TEST(
    SavorPredictBattlePredictionBatchRun,
    ScenarioResolvesItsDefaultProfileDuringValidation) {
    TempTree tree("savor_predict_batch_scenario_");
    const auto db_root = tree.path() / "db";
    const auto std_json_dir = tree.path() / "std-json";
    ASSERT_TRUE(std::filesystem::create_directories(db_root));
    ASSERT_TRUE(std::filesystem::create_directories(std_json_dir));

    BattlePredictionBatchRunOptions options;
    options.source_exec_job_ids = {147884};
    options.db_root = db_root;
    options.scenario_name = "first-battle-soldiers";
    options.action_view_std_json_dir = std_json_dir;
    options.resource_inputs = ready_resource_inputs();
    options.run_root = tree.path() / "run";
    options.run_name = "scenario-default-profile";

    const auto errors =
        validate_battle_prediction_batch_run_options(options);

    EXPECT_TRUE(errors.empty())
        << (errors.empty() ? std::string{} : errors.front());
}

TEST(
    SavorPredictBattlePredictionBatchRun,
    StrictPreflightWritesFailureArtifactsButInvokesNoPredictions) {
    TempTree tree("savor_predict_batch_preflight_");
    const auto db_root = tree.path() / "db";
    const auto std_json_dir = tree.path() / "std-json";
    const auto run_root = tree.path() / "run";
    ASSERT_TRUE(std::filesystem::create_directories(db_root));
    ASSERT_TRUE(std::filesystem::create_directories(std_json_dir));
    {
        std::ofstream analysis(
            db_root / "analysis.db",
            std::ios::binary | std::ios::trunc);
        std::ofstream state(
            db_root / "state.db",
            std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(analysis);
        ASSERT_TRUE(state);
        analysis << "deliberately-invalid-analysis";
        state << "deliberately-invalid-state";
    }

    BattlePredictionBatchRunOptions options;
    options.source_exec_job_ids = {101, 202};
    options.db_root = db_root;
    options.profile_name = "first-battle-soldiers";
    options.action_view_std_json_dir = std_json_dir;
    options.resource_inputs = ready_resource_inputs();
    options.run_root = run_root;
    options.run_name = "strict-preflight";
    options.require_complete = true;

    std::ostringstream progress;
    std::ostringstream err;
    const auto result =
        run_battle_prediction_batch(options, progress, err);

    EXPECT_FALSE(result.preflight_succeeded);
    EXPECT_TRUE(result.artifacts_valid);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.recommended_exit_code, 1);
    EXPECT_FALSE(
        result.database_identity.used_database_fingerprint.empty());
    EXPECT_FALSE(result.database_identity.snapshot_manifest_present);
    EXPECT_TRUE(
        contains_error(result.errors, "Strict preflight failed"));
    ASSERT_EQ(result.jobs.size(), 2u);
    for (const auto& job : result.jobs) {
        EXPECT_EQ(
            job.invocation_status,
            BattlePredictionBatchInvocationStatus::NotRun);
        EXPECT_EQ(
            job.prediction_status,
            BattlePredictionBatchPredictionStatus::NotRun);
        EXPECT_FALSE(job.prediction_json_valid);
        EXPECT_FALSE(
            std::filesystem::exists(run_root / job.prediction_path));
        EXPECT_TRUE(std::filesystem::is_regular_file(
            run_root / job.stderr_path));
    }
    EXPECT_TRUE(std::filesystem::is_empty(run_root / "predictions"));

    EXPECT_TRUE(std::filesystem::is_regular_file(result.request_path));
    EXPECT_TRUE(std::filesystem::is_regular_file(result.manifest_path));
    EXPECT_TRUE(std::filesystem::is_regular_file(result.summary_csv_path));
    EXPECT_TRUE(std::filesystem::is_regular_file(result.summary_text_path));

    const auto request = read_text_file(result.request_path);
    EXPECT_NE(request.find("\"schema_version\": 2"), std::string::npos);
    EXPECT_NE(request.find("\"resource_inputs\": {"), std::string::npos);
    EXPECT_NE(
        request.find("\"provider_kind\":\"direct_spice\""),
        std::string::npos);
    EXPECT_NE(
        request.find("\"normalized_relative_path\""),
        std::string::npos);
    EXPECT_NE(request.find("\"source_path\""), std::string::npos);
    EXPECT_NE(request.find("\"parser_identity\""), std::string::npos);
    EXPECT_NE(request.find("\"diagnostic_count\""), std::string::npos);
    EXPECT_EQ(
        request.find("\"action_view_std_json_dir\""),
        std::string::npos);
    EXPECT_NE(
        request.find("\"source_exec_job_ids\": [101, 202]"),
        std::string::npos);
    EXPECT_NE(request.find("\"require_complete\": true"), std::string::npos);

    const auto manifest = read_text_file(result.manifest_path);
    EXPECT_NE(manifest.find("\"schema_version\": 2"), std::string::npos);
    EXPECT_NE(manifest.find("\"resource_inputs\": {"), std::string::npos);
    EXPECT_EQ(
        manifest.find("\"action_view_std_json_dir\""),
        std::string::npos);
    EXPECT_NE(
        manifest.find("\"preflight_succeeded\": false"),
        std::string::npos);
    EXPECT_NE(manifest.find("\"artifacts_valid\": true"), std::string::npos);
    EXPECT_NE(manifest.find("\"success\": false"), std::string::npos);
    EXPECT_NE(
        manifest.find("\"used_database_fingerprint\": \""),
        std::string::npos);
    EXPECT_NE(
        manifest.find("\"snapshot_manifest_present\": false"),
        std::string::npos);
    EXPECT_NE(
        manifest.find("\"prediction_status\": \"not_run\""),
        std::string::npos);

    const auto summary_csv = read_text_file(result.summary_csv_path);
    EXPECT_NE(
        summary_csv.find(
            "source_exec_job_id,invocation_status,prediction_status"),
        std::string::npos);
    EXPECT_NE(summary_csv.find("101,not_run,not_run"), std::string::npos);
    EXPECT_NE(summary_csv.find("202,not_run,not_run"), std::string::npos);

    const auto summary_text = read_text_file(result.summary_text_path);
    EXPECT_NE(
        summary_text.find("strict_preflight: failed"),
        std::string::npos);
    EXPECT_NE(
        summary_text.find("job 101 invocation=not_run prediction=not_run"),
        std::string::npos);
}

TEST(
    SavorPredictBattlePredictionBatchRun,
    LegacyProviderArtifactsRetainExplicitStdJsonDirectory) {
    TempTree tree("savor_predict_batch_legacy_artifacts_");
    const auto db_root = tree.path() / "db";
    const auto std_json_dir = tree.path() / "std-json";
    const auto run_root = tree.path() / "run";
    ASSERT_TRUE(std::filesystem::create_directories(db_root));
    ASSERT_TRUE(std::filesystem::create_directories(std_json_dir));
    {
        std::ofstream analysis(
            db_root / "analysis.db",
            std::ios::binary | std::ios::trunc);
        std::ofstream state(
            db_root / "state.db",
            std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(analysis);
        ASSERT_TRUE(state);
        analysis << "deliberately-invalid-analysis";
        state << "deliberately-invalid-state";
    }

    BattlePredictionBatchRunOptions options;
    options.source_exec_job_ids = {101};
    options.db_root = db_root;
    options.profile_name = "first-battle-soldiers";
    options.action_view_std_json_dir = std_json_dir;
    options.resource_inputs = ready_resource_inputs(
        BattlePredictorResourceProviderKind::LegacyStdJsonDirectMld);
    options.run_root = run_root;
    options.run_name = "legacy-artifacts";

    std::ostringstream progress;
    std::ostringstream err;
    const auto result =
        run_battle_prediction_batch(options, progress, err);

    ASSERT_TRUE(std::filesystem::is_regular_file(result.request_path));
    ASSERT_TRUE(std::filesystem::is_regular_file(result.manifest_path));
    const auto request = read_text_file(result.request_path);
    const auto manifest = read_text_file(result.manifest_path);
    EXPECT_NE(
        request.find("\"provider_kind\":\"legacy_std_json_direct_mld\""),
        std::string::npos);
    EXPECT_NE(
        request.find("\"action_view_std_json_dir\""),
        std::string::npos);
    EXPECT_NE(
        manifest.find("\"provider_kind\":\"legacy_std_json_direct_mld\""),
        std::string::npos);
    EXPECT_NE(
        manifest.find("\"action_view_std_json_dir\""),
        std::string::npos);
}

} // namespace
