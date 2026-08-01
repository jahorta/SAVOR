#include <gtest/gtest.h>

#include <chrono>
#include <string>
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
        .slots = {
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
    snapshot.slots[1].ready = true;
    snapshot.slots[1].starting = false;
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
    impossible.slots[1].starting = false;
    impossible.slots[1].exhausted = true;
    impossible.slots[1].terminal_diagnostic =
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
