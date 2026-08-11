#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "Cli.h"
#include "WorkerStartupBarrier.h"

namespace {

bool ParseSeedProbeArgs(
    std::initializer_list<const char*> args,
    savor::e2e::CliOptions* options,
    std::string* error) {
    std::vector<std::string> storage(args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& value : storage) {
        argv.push_back(value.data());
    }
    return savor::e2e::ParseArgs(
        static_cast<int>(argv.size()),
        argv.data(),
        options,
        error);
}

savor::e2e::FleetStartupSnapshot PartialFleet() {
    return {
        .desired = 2,
        .ready = 1,
        .starting = 1,
        .worker_slots = {
            {
                .worker_id = 0,
                .attempt_count = 1,
                .maximum_attempts = 3,
                .ready = true,
            },
            {
                .worker_id = 1,
                .attempt_count = 1,
                .maximum_attempts = 3,
                .starting = true,
            },
        },
    };
}

savor::e2e::FleetStartupSnapshot ReadyFleet() {
    auto snapshot = PartialFleet();
    snapshot.ready = 2;
    snapshot.starting = 0;
    snapshot.worker_slots[1].ready = true;
    snapshot.worker_slots[1].starting = false;
    return snapshot;
}

TEST(SeedProbeWorkerStartupCli, BarrierDefaultsOffAndParsesExplicitly) {
    savor::e2e::CliOptions defaults;
    std::string error;
    ASSERT_TRUE(ParseSeedProbeArgs(
        {
            "SavorE2E",
            "--scenario", "seedprobe",
            "--iso", ".",
            "--dolphin-base-dir", ".",
            "--savestate-file", ".",
            "--dtm-file", ".",
        },
        &defaults,
        &error)) << error;
    EXPECT_FALSE(defaults.wait_for_workers_ready);

    savor::e2e::CliOptions explicit_barrier;
    ASSERT_TRUE(ParseSeedProbeArgs(
        {
            "SavorE2E",
            "--scenario", "seedprobe",
            "--iso", ".",
            "--dolphin-base-dir", ".",
            "--savestate-file", ".",
            "--dtm-file", ".",
            "--wait-for-workers-ready",
        },
        &explicit_barrier,
        &error)) << error;
    EXPECT_TRUE(explicit_barrier.wait_for_workers_ready);
}

TEST(TasMovieEstablishmentCli, AcceptsOnlyTheEstablishmentInputs) {
    savor::e2e::CliOptions options;
    std::string error;
    ASSERT_TRUE(ParseSeedProbeArgs(
        {
            "SavorE2E",
            "--scenario", "tasmovie",
            "--iso", ".",
            "--dolphin-base-dir", ".",
            "--dtm-file", ".",
        },
        &options,
        &error)) << error;
    ASSERT_EQ(options.scenarios.size(), 1u);
    EXPECT_EQ(options.scenarios.front(), "tasmovie");
    EXPECT_TRUE(options.savestate_file.empty());
    EXPECT_FALSE(options.tasmovie_rtc.has_value());
}

TEST(TasMovieEstablishmentCli, RejectsCombinationRepeatAndRtcModes) {
    savor::e2e::CliOptions options;
    std::string error;
    EXPECT_FALSE(ParseSeedProbeArgs(
        {
            "SavorE2E", "--scenario", "tasmovie",
            "--scenario", "seedprobe", "--iso", ".",
            "--dolphin-base-dir", ".", "--dtm-file", ".",
            "--savestate-file", ".",
        },
        &options,
        &error));
    EXPECT_NE(error.find("must run alone"), std::string::npos);

    error.clear();
    EXPECT_FALSE(ParseSeedProbeArgs(
        {
            "SavorE2E", "--scenario", "tasmovie", "--repeat", "2",
            "--iso", ".", "--dolphin-base-dir", ".",
            "--dtm-file", ".",
        },
        &options,
        &error));
    EXPECT_NE(error.find("--repeat 1"), std::string::npos);

    error.clear();
    EXPECT_FALSE(ParseSeedProbeArgs(
        {
            "SavorE2E", "--scenario", "tasmovie",
            "--tasmovie-rtc", "0", "--iso", ".",
            "--dolphin-base-dir", ".", "--dtm-file", ".",
        },
        &options,
        &error));
    EXPECT_NE(error.find("does not accept RTC"), std::string::npos);

    error.clear();
    EXPECT_FALSE(ParseSeedProbeArgs(
        {
            "SavorE2E", "--scenario", "tasmovie",
            "--worker-count", "2", "--iso", ".",
            "--dolphin-base-dir", ".", "--dtm-file", ".",
        },
        &options,
        &error));
    EXPECT_NE(error.find("exactly one worker"), std::string::npos);
}

TEST(TasMovieEstablishmentCli, TasMovieScenariosRemainExcludedFromAll) {
    savor::e2e::CliOptions options;
    std::string error;
    ASSERT_TRUE(ParseSeedProbeArgs(
        {
            "SavorE2E", "--scenario", "all", "--iso", ".",
            "--dolphin-base-dir", ".", "--savestate-file", ".",
        },
        &options,
        &error)) << error;
    EXPECT_EQ(
        std::find(
            options.scenarios.begin(), options.scenarios.end(), "tasmovie"),
        options.scenarios.end());
    ASSERT_EQ(options.scenarios.size(), 1u);
    EXPECT_EQ(options.scenarios.front(), "seedprobe");
    EXPECT_EQ(
        std::find(
            options.scenarios.begin(), options.scenarios.end(),
            "tasmovie_with_validation"),
        options.scenarios.end());
    EXPECT_EQ(
        std::find(
            options.scenarios.begin(), options.scenarios.end(),
            "tasmovie_seedprobe"),
        options.scenarios.end());

    for (const auto* removed : {
             "seedprobe_battle", "battle_macro_probe",
             "navigation_context"}) {
        error.clear();
        EXPECT_FALSE(ParseSeedProbeArgs(
            {"SavorE2E", "--scenario", removed},
            &options,
            &error));
        EXPECT_NE(error.find("unknown --scenario"), std::string::npos);
    }
}

TEST(BattleCli, RequiresApprovedDtmExactRtcAndRejectsExternalSavestate) {
    savor::e2e::CliOptions options;
    std::string error;
    ASSERT_TRUE(ParseSeedProbeArgs(
        {
            "SavorE2E", "--scenario", "battle",
            "--tasmovie-rtc", "0", "--iso", ".",
            "--dolphin-base-dir", ".", "--dtm-file", ".",
        },
        &options,
        &error)) << error;
    ASSERT_EQ(options.scenarios.size(), 1u);
    EXPECT_EQ(options.scenarios.front(), "battle");
    EXPECT_TRUE(options.savestate_file.empty());
    EXPECT_EQ(options.dtm_file, std::filesystem::path("."));
    ASSERT_TRUE(options.tasmovie_rtc.has_value());
    EXPECT_EQ(*options.tasmovie_rtc, 0);

    const auto reject = [](std::initializer_list<const char*> extra) {
        std::vector<std::string> storage{
            "SavorE2E", "--scenario", "battle", "--iso", ".",
            "--dolphin-base-dir", ".", "--dtm-file", ".",
        };
        storage.insert(storage.end(), extra.begin(), extra.end());
        std::vector<char*> argv;
        for (auto& value : storage) argv.push_back(value.data());
        savor::e2e::CliOptions parsed;
        std::string parse_error;
        return !savor::e2e::ParseArgs(
            static_cast<int>(argv.size()), argv.data(), &parsed,
            &parse_error);
    };

    EXPECT_TRUE(reject({}));
    EXPECT_TRUE(reject({"--tasmovie-rtc", "0", "--savestate-file", "."}));
    EXPECT_TRUE(reject({
        "--tasmovie-rtc-min", "0", "--tasmovie-rtc-max", "1"}));
    EXPECT_TRUE(reject({"--tasmovie-rtc", "0", "--repeat", "2"}));
    EXPECT_TRUE(reject({
        "--tasmovie-rtc", "0", "--scenario", "seedprobe",
        "--savestate-file", "."}));
}

TEST(E2ePreparedCheckpointCli, CatalogEnablesSeedProbeAndBattleOnly) {
    const auto* seedprobe =
        savor::e2e::FindE2eScenarioDescriptor("seedprobe");
    const auto* battle = savor::e2e::FindE2eScenarioDescriptor("battle");
    const auto* tasmovie = savor::e2e::FindE2eScenarioDescriptor("tasmovie");
    ASSERT_NE(seedprobe, nullptr);
    ASSERT_NE(battle, nullptr);
    ASSERT_NE(tasmovie, nullptr);
    const auto prepared = savor::e2e::EntrySourceBit(
        savor::e2e::E2eScenarioEntrySource::PreparedSterilizedCheckpoint);
    EXPECT_NE(seedprobe->supported_entry_sources & prepared, 0u);
    EXPECT_NE(battle->supported_entry_sources & prepared, 0u);
    EXPECT_EQ(tasmovie->supported_entry_sources & prepared, 0u);
}

TEST(E2ePreparedCheckpointCli, AcceptsSeedProbeAndBattleAndRejectsMixedInputs) {
    const auto root = std::filesystem::temp_directory_path()
        / ("savor-e2e-prepared-cli-"
            + std::to_string(std::chrono::steady_clock::now()
                .time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(root));
    for (const auto* name : {
             "execution.db", "state.db", "analysis.db", "authoring.db",
             "ui_read.db", "archive.db"}) {
        std::ofstream(root / name, std::ios::binary).put('\0');
    }

    const auto parse = [&](std::string scenario,
                           std::initializer_list<std::string> extra,
                           savor::e2e::CliOptions* options,
                           std::string* error) {
        std::vector<std::string> storage{
            "SavorE2E", "--scenario", std::move(scenario),
            "--iso", ".", "--dolphin-base-dir", ".",
            "--workspace-root", root.string(),
            "--source-savestate-id", "7",
        };
        storage.insert(storage.end(), extra.begin(), extra.end());
        std::vector<char*> argv;
        for (auto& value : storage) argv.push_back(value.data());
        return savor::e2e::ParseArgs(
            static_cast<int>(argv.size()), argv.data(), options, error);
    };

    savor::e2e::CliOptions options;
    std::string error;
    EXPECT_TRUE(parse("seedprobe", {}, &options, &error)) << error;
    error.clear();
    EXPECT_TRUE(parse("battle", {}, &options, &error)) << error;
    error.clear();
    EXPECT_FALSE(parse("tasmovie", {}, &options, &error));
    EXPECT_NE(error.find("does not support entry source"), std::string::npos);
    error.clear();
    EXPECT_FALSE(parse(
        "battle", {"--dtm-file", "."}, &options, &error));
    EXPECT_NE(error.find("rejects"), std::string::npos);
    error.clear();
    EXPECT_FALSE(parse(
        "seedprobe", {"--savestate-file", "."}, &options, &error));
    EXPECT_NE(error.find("rejects"), std::string::npos);

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
}

TEST(TasMovieSeedProbeCli, AcceptsRtcEndpointsWorkersAndSeedProbeOptions) {
    for (const auto* rtc : {"0", "4294967295"}) {
        for (const auto* workers : {"1", "30"}) {
            savor::e2e::CliOptions options;
            std::string error;
            ASSERT_TRUE(ParseSeedProbeArgs(
                {
                    "SavorE2E", "--scenario", "tasmovie_seedprobe",
                    "--tasmovie-rtc", rtc,
                    "--worker-count", workers,
                    "--seedprobe-samples-per-axis", "5",
                    "--seedprobe-combo-attempts-per-target", "32",
                    "--iso", ".", "--dolphin-base-dir", ".",
                    "--dtm-file", ".",
                },
                &options,
                &error)) << error;
            EXPECT_EQ(options.scenarios.front(), "tasmovie_seedprobe");
            EXPECT_EQ(options.worker_count, std::stoi(workers));
            EXPECT_EQ(options.seedprobe_samples_per_axis, 5);
            EXPECT_EQ(options.seedprobe_combo_attempts_per_target, 32);
        }
    }
}

TEST(TasMovieSeedProbeCli, RejectsInvalidCompositionArguments) {
    const auto reject = [](std::initializer_list<const char*> extra) {
        std::vector<std::string> storage{
            "SavorE2E", "--scenario", "tasmovie_seedprobe",
            "--iso", ".", "--dolphin-base-dir", ".",
            "--dtm-file", ".",
        };
        storage.insert(storage.end(), extra.begin(), extra.end());
        std::vector<char*> argv;
        for (auto& value : storage) argv.push_back(value.data());
        savor::e2e::CliOptions options;
        std::string error;
        return !savor::e2e::ParseArgs(
            static_cast<int>(argv.size()), argv.data(), &options, &error);
    };

    EXPECT_TRUE(reject({}));
    EXPECT_TRUE(reject({"--tasmovie-rtc-min", "0", "--tasmovie-rtc-max", "1"}));
    EXPECT_TRUE(reject({"--tasmovie-rtc", "0", "--tasmovie-rtc-min", "0"}));
    EXPECT_TRUE(reject({"--tasmovie-rtc", "-1"}));
    EXPECT_TRUE(reject({"--tasmovie-rtc", "4294967296"}));
    EXPECT_TRUE(reject({"--tasmovie-rtc", "0", "--repeat", "2"}));
    EXPECT_TRUE(reject({"--tasmovie-rtc", "0", "--worker-count", "31"}));
    EXPECT_TRUE(reject({"--tasmovie-rtc", "0", "--savestate-file", "."}));
    EXPECT_TRUE(reject({
        "--tasmovie-rtc", "0", "--scenario", "seedprobe",
        "--savestate-file", "."}));
}

TEST(TasMovieWithValidationCli, AcceptsExactFullU32RtcDomain) {
    for (const auto* rtc : {"0", "4294967295"}) {
        savor::e2e::CliOptions options;
        std::string error;
        ASSERT_TRUE(ParseSeedProbeArgs(
            {
                "SavorE2E", "--scenario", "tasmovie_with_validation",
                "--tasmovie-rtc", rtc, "--iso", ".",
                "--dolphin-base-dir", ".", "--dtm-file", ".",
            },
            &options,
            &error)) << error;
        ASSERT_TRUE(options.tasmovie_rtc.has_value());
        EXPECT_EQ(
            *options.tasmovie_rtc,
            rtc == std::string_view("0")
                ? 0
                : static_cast<std::int64_t>(
                    std::numeric_limits<std::uint32_t>::max()));
    }
}

TEST(TasMovieWithValidationCli, RejectsMissingRangeConflictingAndInvalidRtc) {
    const auto reject = [](std::initializer_list<const char*> extra) {
        std::vector<std::string> storage{
            "SavorE2E", "--scenario", "tasmovie_with_validation",
            "--iso", ".", "--dolphin-base-dir", ".", "--dtm-file", ".",
        };
        storage.insert(storage.end(), extra.begin(), extra.end());
        std::vector<char*> argv;
        for (auto& value : storage) argv.push_back(value.data());
        savor::e2e::CliOptions options;
        std::string error;
        return !savor::e2e::ParseArgs(
            static_cast<int>(argv.size()), argv.data(), &options, &error);
    };

    EXPECT_TRUE(reject({}));
    EXPECT_TRUE(reject({"--tasmovie-rtc-min", "0", "--tasmovie-rtc-max", "1"}));
    EXPECT_TRUE(reject({"--tasmovie-rtc", "0", "--tasmovie-rtc-min", "0"}));
    EXPECT_TRUE(reject({"--tasmovie-rtc", "-1"}));
    EXPECT_TRUE(reject({"--tasmovie-rtc", "4294967296"}));
    EXPECT_TRUE(reject({"--tasmovie-rtc", "0", "--repeat", "2"}));
    EXPECT_TRUE(reject({"--tasmovie-rtc", "0", "--worker-count", "2"}));
    EXPECT_TRUE(reject({
        "--tasmovie-rtc", "0", "--scenario", "seedprobe",
        "--savestate-file", "."}));
}

TEST(SeedProbeWorkerStartupBarrier, HoldsClaimsUntilFullFleetIsReady) {
    bool claims_paused = false;
    int pause_calls = 0;
    int snapshot_calls = 0;
    int claims_before_release = 0;
    std::vector<std::string> events;
    const auto set_paused = [&](bool paused) {
        claims_paused = paused;
        ++pause_calls;
    };

    savor::e2e::ArmInitialWorkerPoolBarrier(
        true,
        set_paused,
        [&](const std::string& line) { events.push_back(line); });
    ASSERT_TRUE(claims_paused);

    const auto result = savor::e2e::WaitForInitialWorkerPool(
        true,
        std::chrono::milliseconds(1),
        [&]() {
            if (!claims_paused) {
                ++claims_before_release;
            }
            return snapshot_calls++ == 0
                ? PartialFleet()
                : ReadyFleet();
        },
        set_paused,
        [&](const std::string& line) { events.push_back(line); });

    EXPECT_TRUE(result.satisfied) << result.diagnostic;
    EXPECT_TRUE(result.snapshot.full_pool_ready());
    EXPECT_FALSE(claims_paused);
    EXPECT_EQ(claims_before_release, 0);
    EXPECT_EQ(pause_calls, 2);
    ASSERT_GE(events.size(), 4u);
    EXPECT_NE(events.front().find("enabled=1"), std::string::npos);
    EXPECT_NE(events.back().find("barrier-complete"), std::string::npos);
}

TEST(SeedProbeWorkerStartupBarrier, FailsImmediatelyOnExhaustedRequiredSlot) {
    auto impossible = PartialFleet();
    impossible.starting = 0;
    impossible.exhausted = 1;
    impossible.worker_slots[1].starting = false;
    impossible.worker_slots[1].exhausted = true;
    impossible.worker_slots[1].terminal_diagnostic =
        "worker 1 capability preflight failed";

    bool claims_paused = false;
    savor::e2e::ArmInitialWorkerPoolBarrier(
        true,
        [&](bool paused) { claims_paused = paused; });
    const auto result = savor::e2e::WaitForInitialWorkerPool(
        true,
        std::chrono::milliseconds(1),
        [&]() { return impossible; },
        [&](bool paused) { claims_paused = paused; });

    EXPECT_FALSE(result.satisfied);
    EXPECT_TRUE(claims_paused);
    EXPECT_NE(
        result.diagnostic.find(
            "worker 1 capability preflight failed"),
        std::string::npos)
        << result.diagnostic;
}

} // namespace
