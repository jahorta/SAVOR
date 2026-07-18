#include <gtest/gtest.h>

#include "Runner/IPC/WorkerWireWriter.h"
#include "Runner/Script/ScriptProgress.h"
#include "NativeHookSemantics.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using namespace savor;
using namespace savor::capture_format;
using namespace savor::probe;
using namespace savor::progress;

Field scalar(std::string name, std::uint64_t value)
{
    Field field;
    field.name = std::move(name);
    field.type = FieldType::Unsigned;
    field.value = value;
    return field;
}

TEST(SavorProbeProgress, InstallsTypedProgressOnlyBattleProbes)
{
    Profile profile;
    profile.name = "progress-only";
    append_battle_progress_probes(profile);

    ASSERT_EQ(profile.probes.size(), 5u);
    const std::unordered_set<std::string> expected{
        "battle_progress.attack_damage",
        "battle_progress.counterattack",
        "battle_progress.death",
        "battle_progress.item_drop",
        "battle_progress.instructions",
    };
    for (const auto& probe : profile.probes) {
        EXPECT_TRUE(expected.contains(probe.id));
        EXPECT_EQ(probe.group, "battle_progress");
        EXPECT_EQ(probe.kind, ProbeKind::Pc);
        EXPECT_TRUE(has_subscription(probe.subscriptions, Subscription::Progress));
        EXPECT_FALSE(has_subscription(probe.subscriptions, Subscription::Capture));
        EXPECT_FALSE(has_subscription(probe.subscriptions, Subscription::Control));
        EXPECT_FALSE(probe.progress_formatter.empty());
    }
}

TEST(SavorProbeProgress, SerializesHashPinnedEffectiveProfileWithBuiltIns)
{
    Profile profile;
    profile.name = "effective-progress";
    profile.expected_module_sha256 = std::string(64, 'b');
    append_battle_progress_probes(profile);

    const auto serialized = serialize_profile_json(profile);
    const auto parsed = parse_profile_json(serialized);
    ASSERT_TRUE(parsed.profile.has_value()) << format_profile_errors(parsed);
    EXPECT_EQ(parsed.profile->expected_module_sha256, profile.expected_module_sha256);
    EXPECT_EQ(parsed.profile->probes.size(), profile.probes.size());
    EXPECT_NE(serialized.find("battle_progress.attack_damage"), std::string::npos);
}

TEST(SavorProbeProgress, FormatsCapturedDamageAndPreservesRecordPolicy)
{
    Event event;
    event.kind = EventKind::Progress;
    event.probe_id = "battle_progress.attack_damage";
    event.fields = {
        scalar("attacker_slot", 0),
        scalar("target_slot", 4),
        scalar("damage", 37),
        scalar("attacker_id", 0),
        scalar("target_id", 0),
    };

    const auto formatted = format_battle_progress(event, false);
    ASSERT_TRUE(formatted.has_value());
    EXPECT_NE(formatted->text.find("attacks"), std::string::npos);
    EXPECT_NE(formatted->text.find("37 damage"), std::string::npos);
    EXPECT_FALSE(formatted->record_progress);

    event.fields.pop_back();
    EXPECT_FALSE(format_battle_progress(event, true).has_value());
}

TEST(SavorProbeNativeHooks, DispatchesOneAuthoritativeEventAndCombinesControlResults)
{
    constexpr std::uint64_t supplied = 0x123456789abcdef0ull;
    for (const std::size_t size : { 1u, 2u, 4u, 8u }) {
        const NativeMemcheckEvent event{
            supplied,
            0x803469A8u,
            true,
            size,
            0x8025ECD8u,
        };
        std::uint32_t foreign_calls = 0;
        std::uint32_t probe_calls = 0;
        const bool should_break = dispatch_memcheck_action_once(
            event,
            [&](const NativeMemcheckEvent& observed) {
                ++foreign_calls;
                EXPECT_EQ(observed.value, supplied);
                EXPECT_EQ(observed.size, size);
                EXPECT_TRUE(observed.write);
                return false;
            },
            [&](const NativeMemcheckEvent& observed) {
                ++probe_calls;
                EXPECT_EQ(observed.value, supplied);
                EXPECT_EQ(observed.address, 0x803469A8u);
                EXPECT_EQ(observed.size, size);
                EXPECT_TRUE(observed.write);
                EXPECT_EQ(observed.pc, 0x8025ECD8u);
                return false;
            });
        EXPECT_FALSE(should_break);
        EXPECT_EQ(foreign_calls, 1u);
        EXPECT_EQ(probe_calls, 1u);
    }

    const NativeMemcheckEvent read_event{ supplied, 0x80001000u, false, 4, 0x80002000u };
    EXPECT_FALSE(dispatch_memcheck_action_once(
        read_event,
        [](const NativeMemcheckEvent&) { return false; },
        [](const NativeMemcheckEvent& observed) {
            EXPECT_FALSE(observed.write);
            return false;
        }));

    for (const bool foreign_control : { false, true }) {
        for (const bool probe_control : { false, true }) {
            std::uint32_t probe_calls = 0;
            const bool should_break = dispatch_memcheck_action_once(
                read_event,
                [=](const NativeMemcheckEvent&) { return foreign_control; },
                [&](const NativeMemcheckEvent&) {
                    ++probe_calls;
                    return probe_control;
                });
            EXPECT_EQ(should_break, foreign_control || probe_control);
            EXPECT_EQ(probe_calls, 1u);
        }
    }
}

TEST(WorkerWireWriter, SerializesMultipartMessagesAcrossConcurrentProducers)
{
    std::vector<std::uint8_t> bytes;
    WorkerWireWriter writer([&bytes](const void* data, std::size_t size) {
        const auto* begin = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), begin, begin + size);
        std::this_thread::yield();
        return true;
    });

    constexpr std::uint32_t kThreadCount = 8;
    constexpr std::uint32_t kMessagesPerThread = 100;
    std::atomic<bool> failed{ false };
    std::vector<std::thread> threads;
    for (std::uint32_t thread = 0; thread < kThreadCount; ++thread) {
        threads.emplace_back([thread, &writer, &failed] {
            for (std::uint32_t index = 0; index < kMessagesPerThread; ++index) {
                const std::uint32_t id = thread * kMessagesPerThread + index;
                const std::uint32_t complement = ~id;
                if (!writer.write_parts({
                        WorkerWirePart{ &id, sizeof(id) },
                        WorkerWirePart{ &complement, sizeof(complement) },
                    })) {
                    failed.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& thread : threads)
        thread.join();
    ASSERT_FALSE(failed.load(std::memory_order_relaxed));

    constexpr std::size_t kMessageBytes = sizeof(std::uint32_t) * 2;
    ASSERT_EQ(bytes.size(), kThreadCount * kMessagesPerThread * kMessageBytes);
    std::unordered_set<std::uint32_t> ids;
    for (std::size_t offset = 0; offset < bytes.size(); offset += kMessageBytes) {
        std::uint32_t id = 0;
        std::uint32_t complement = 0;
        std::memcpy(&id, bytes.data() + offset, sizeof(id));
        std::memcpy(&complement, bytes.data() + offset + sizeof(id), sizeof(complement));
        EXPECT_EQ(complement, ~id);
        ids.insert(id);
    }
    EXPECT_EQ(ids.size(), kThreadCount * kMessagesPerThread);
}

} // namespace
