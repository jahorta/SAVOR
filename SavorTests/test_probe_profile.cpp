#include <gtest/gtest.h>

#include "AddressProgramEvaluator.h"
#include "CaptureRetention.h"
#include "ProbeEvent.h"
#include "ProbeDispatchPolicy.h"
#include "ProbeProfile.h"
#include "ProbeRuntime.h"
#include "ProbeWindowState.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using namespace savor::probe;

TEST(SavorProbeProfile, ParsesUnifiedSubscribersPredicatesWindowsAndSampling)
{
    const std::string json = R"json(
{
  "schema": "savor.capture.profile/1",
  "name": "unified-probe-test",
  "revision": 7,
  "expected_module_sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "limits": {
    "queue_bytes": 1048576,
    "max_events": 128,
    "progress_events": 32
  },
  "probes": [
    {
      "id": "shared.pc",
      "group": "shared",
      "kind": "pc",
      "address": "0x80001000",
      "subscriptions": ["capture", "progress", "control"],
      "frame_clock": true,
      "progress_formatter": "unit",
      "sampling": {"mode": "burst", "period": 5, "length": 2},
      "predicate": [
        {"op": "gpr", "register": 3},
        {"op": "constant", "value": 5},
        {"op": "eq"}
      ],
      "samples": [
        {"name": "actor", "type": "gpr", "register": 3, "width": 4},
        {"name": "stack", "type": "stack_trace", "max_frames": 8},
        {
          "name": "threads",
          "type": "linked_list",
          "width": 4,
          "program": [7, 132, 26, 49, 128, 0],
          "next_offset": 4,
          "max_nodes": 128,
          "fields": [
            {"name": "callback", "offset": 0, "width": 4},
            {"name": "next", "offset": 4, "width": 4}
          ]
        }
      ]
    },
    {
      "id": "seed.write",
      "group": "shared",
      "kind": "memory",
      "address": "0x8030a000",
      "size": 4,
      "access": "write",
      "subscriptions": ["capture"]
    }
  ],
  "windows": [
    {
      "id": "turn",
      "open_probe": "shared.pc",
      "close_on_control": true,
      "open_frame": 2,
      "event_limit": 100,
      "frame_limit": 30
    }
  ]
}

)json";

    const auto result = parse_profile_json(json);
    ASSERT_TRUE(result.profile.has_value()) << format_profile_errors(result);
    ASSERT_EQ(result.profile->probes.size(), 2u);
    const auto& shared = result.profile->probes[0];
    EXPECT_TRUE(has_subscription(shared.subscriptions, Subscription::Capture));
    EXPECT_TRUE(has_subscription(shared.subscriptions, Subscription::Progress));
    EXPECT_TRUE(has_subscription(shared.subscriptions, Subscription::Control));
    EXPECT_TRUE(shared.frame_clock);
    EXPECT_EQ(shared.sampling.mode, SamplingMode::Burst);
    EXPECT_EQ(shared.sampling.burst_period, 5u);
    EXPECT_EQ(shared.sampling.burst_length, 2u);
    EXPECT_EQ(shared.predicate.size(), 3u);
    ASSERT_EQ(shared.samples.size(), 3u);
    EXPECT_EQ(shared.samples[1].kind, SampleKind::StackTrace);
    EXPECT_EQ(shared.samples[1].max_frames, 8u);
    EXPECT_EQ(shared.samples[2].kind, SampleKind::LinkedList);
    EXPECT_EQ(shared.samples[2].max_nodes, 128u);
    ASSERT_EQ(result.profile->windows.size(), 1u);
    EXPECT_TRUE(result.profile->windows.front().close_on_control);
}

TEST(SavorProbeProfile, RejectsRemovedUnknownAndTrailingFields)
{
    const auto removed_slot = parse_profile_json(R"({
      "schema":"savor.capture.profile/1","name":"removed-slot",
      "limits":{"event_slot_bytes":65536},"probes":[]})");
    EXPECT_FALSE(removed_slot.profile.has_value());
    EXPECT_NE(format_profile_errors(removed_slot).find("event_slot_bytes"), std::string::npos);

    const auto removed_scope = parse_profile_json(R"({
      "schema":"savor.capture.profile/1","name":"removed-scope","probes":[{
        "id":"p","kind":"pc","address":1,"subscriptions":["capture"],
        "predicate":[{"op":"pc","scope":"input_macro"}]
      }]})");
    EXPECT_FALSE(removed_scope.profile.has_value());
    EXPECT_NE(format_profile_errors(removed_scope).find("scope"), std::string::npos);

    const auto unknown = parse_profile_json(R"({
      "schema":"savor.capture.profile/1","name":"unknown","inert":true,"probes":[]})");
    EXPECT_FALSE(unknown.profile.has_value());
    EXPECT_NE(format_profile_errors(unknown).find("inert"), std::string::npos);

    const auto trailing = parse_profile_json(R"({
      "schema":"savor.capture.profile/1","name":"trailing","probes":[]}) trailing)");
    EXPECT_FALSE(trailing.profile.has_value());
    EXPECT_NE(format_profile_errors(trailing).find("trailing"), std::string::npos);
}

TEST(SavorProbeProfile, ValidatesAddressProgramsAndTracePolicies)
{
    const auto valid = parse_profile_json(R"({
      "schema":"savor.capture.profile/1","name":"address-programs","probes":[
        {"id":"dynamic","kind":"memory","size":4,"access":"write",
         "activate_on_pc":2147487744,"address_program":[6,3,3,4,0,0,0,0],
         "trace":"on_failure","subscriptions":["capture"],"samples":[
           {"name":"value","type":"address_program","program":[7,0,16,0,128,0],
            "width":4,"trace":"always"},
           {"name":"list","type":"linked_list","program":[6,4,0],
            "trace":"off","next_offset":4,"max_nodes":4,
            "fields":[{"name":"next","offset":4,"width":4}]}
         ]}
      ]})");
    ASSERT_TRUE(valid.profile.has_value()) << format_profile_errors(valid);
    ASSERT_EQ(valid.profile->probes.size(), 1u);
    EXPECT_EQ(valid.profile->probes[0].address_trace, AddressTracePolicy::OnFailure);
    ASSERT_EQ(valid.profile->probes[0].samples.size(), 2u);
    EXPECT_EQ(valid.profile->probes[0].samples[0].trace, AddressTracePolicy::Always);
    EXPECT_EQ(valid.profile->probes[0].samples[1].trace, AddressTracePolicy::Off);

    const auto inert_trace = parse_profile_json(R"({
      "schema":"savor.capture.profile/1","name":"inert-trace","probes":[{
        "id":"p","kind":"memory","address":4096,"size":4,"trace":"always",
        "subscriptions":["capture"]
      }]})");
    EXPECT_FALSE(inert_trace.profile.has_value());
    EXPECT_NE(format_profile_errors(inert_trace).find("requires an address_program"), std::string::npos);

    const auto missing_end = parse_profile_json(R"({
      "schema":"savor.capture.profile/1","name":"missing-end","probes":[{
        "id":"p","kind":"memory","size":4,"activate_on_pc":1,
        "address_program":[7,0,16,0,128],"subscriptions":["capture"]
      }]})");
    EXPECT_FALSE(missing_end.profile.has_value());
    EXPECT_NE(format_profile_errors(missing_end).find("termin"), std::string::npos);

    std::string too_many = "[";
    for (int i = 0; i < 64; ++i) {
        if (i != 0) too_many += ',';
        too_many += "3,0,0,0,0";
    }
    too_many += ",0]";
    const auto operation_limit = parse_profile_json(
        "{\"schema\":\"savor.capture.profile/1\",\"name\":\"too-many\",\"probes\":[{"
        "\"id\":\"p\",\"kind\":\"memory\",\"size\":4,\"activate_on_pc\":1,"
        "\"address_program\":" + too_many + ",\"subscriptions\":[\"capture\"]}]}" );
    EXPECT_FALSE(operation_limit.profile.has_value());
    EXPECT_NE(format_profile_errors(operation_limit).find("64 operations"), std::string::npos);
}

TEST(SavorProbeProfile, EnforcesDynamicListRootsAndFlightRecorderOwnership)
{
    const auto valid = parse_profile_json(R"({
      "schema":"savor.capture.profile/1","name":"flight","probes":[
        {"id":"member","kind":"pc","address":1,"subscriptions":["capture"],
         "samples":[{"name":"list","type":"linked_list","address":4096,
          "next_offset":4,"max_nodes":8,"fields":[{"name":"next","offset":4,"width":4}]}]},
        {"id":"trigger","kind":"pc","address":2,"subscriptions":["capture"]}
      ],"flight_recorders":[{"id":"f","member_probes":["member"],
       "pre_events":2,"post_events":3,"trigger_probes":["trigger"]}]
    })");
    ASSERT_TRUE(valid.profile.has_value()) << format_profile_errors(valid);
    ASSERT_EQ(valid.profile->flight_recorders.size(), 1u);

    const auto both_roots = parse_profile_json(R"({
      "schema":"savor.capture.profile/1","name":"both","probes":[{
        "id":"p","kind":"pc","address":1,"subscriptions":["capture"],"samples":[{
          "name":"list","type":"linked_list","address":4096,"program":[7,0,16,0,0,0],
          "next_offset":4,"max_nodes":8,"fields":[{"name":"next","offset":4,"width":4}]
        }]
      }]})");
    EXPECT_FALSE(both_roots.profile.has_value());
    EXPECT_NE(format_profile_errors(both_roots).find("exactly one"), std::string::npos);

    const auto last_n_flight = parse_profile_json(R"({
      "schema":"savor.capture.profile/1","name":"last-n-flight","probes":[
        {"id":"member","kind":"pc","address":1,"subscriptions":["capture"],
         "sampling":{"mode":"last_n","n":2}},
        {"id":"trigger","kind":"pc","address":2,"subscriptions":["capture"]}
      ],"flight_recorders":[{"id":"f","member_probes":["member"],
       "pre_events":1,"post_events":1,"trigger_probes":["trigger"]}]
    })");
    EXPECT_FALSE(last_n_flight.profile.has_value());
    EXPECT_NE(format_profile_errors(last_n_flight).find("LastN"), std::string::npos);
}

TEST(SavorProbeProfile, ParsesEverySamplingModeAndWindowBoundarySource)
{
    const auto result = parse_profile_json(R"({
      "schema":"savor.capture.profile/1","name":"sampling-and-windows",
      "probes":[
        {"id":"every","kind":"pc","address":1,"subscriptions":["capture"]},
        {"id":"nth","kind":"pc","address":2,"subscriptions":["capture"],
         "sampling":{"mode":"every_n","n":3}},
        {"id":"first","kind":"pc","address":3,"subscriptions":["capture"],
         "sampling":{"mode":"first_n","n":4}},
        {"id":"changed","kind":"pc","address":4,"subscriptions":["capture"],
         "sampling":{"mode":"changed_only"}},
        {"id":"ranges","kind":"pc","address":5,"subscriptions":["capture"],
         "sampling":{"mode":"hit_ranges","ranges":[[2,3],[8,13]]}},
        {"id":"last","kind":"pc","address":6,"subscriptions":["capture"],
         "sampling":{"mode":"last_n","n":5}},
        {"id":"burst","kind":"pc","address":7,"subscriptions":["capture"],
         "sampling":{"mode":"burst","period":7,"length":2}}
      ],
      "windows":[
        {"id":"probe","open_probe":"every","close_probe":"nth","event_limit":9},
        {"id":"marker","open_marker":"begin","close_marker":"end","frame_limit":11},
        {"id":"control","open_on_control":true,"close_on_control":true},
        {"id":"frame","open_frame":2,"close_frame":12}
      ]
    })");
    ASSERT_TRUE(result.profile.has_value()) << format_profile_errors(result);
    ASSERT_EQ(result.profile->probes.size(), 7u);
    const std::array expected{
        SamplingMode::EveryHit,
        SamplingMode::EveryN,
        SamplingMode::FirstN,
        SamplingMode::ChangedOnly,
        SamplingMode::HitRanges,
        SamplingMode::LastN,
        SamplingMode::Burst,
    };
    for (std::size_t index = 0; index < expected.size(); ++index)
        EXPECT_EQ(result.profile->probes[index].sampling.mode, expected[index]);
    EXPECT_EQ(result.profile->probes[1].sampling.n, 3u);
    EXPECT_EQ(result.profile->probes[4].sampling.hit_ranges.size(), 2u);
    EXPECT_EQ(result.profile->probes[5].sampling.n, 5u);
    EXPECT_EQ(result.profile->probes[6].sampling.burst_period, 7u);
    EXPECT_EQ(result.profile->windows.size(), 4u);
}

TEST(SavorProbeAddressProgram, TracesRegisterStaticBaseAndPointerOperations)
{
    std::array<std::uint32_t, 32> gprs{};
    gprs[3] = 0x80300000u;
    gprs[4] = 2;
    const std::unordered_map<std::uint32_t, std::uint64_t> memory{
        { 0x80300010u, 0x80400000u },
    };
    const auto read_guest = [&memory](
        std::uint32_t address, SampleWidth width, std::uint64_t& value) {
        const auto found = memory.find(address);
        if (width != SampleWidth::U32 || found == memory.end())
            return false;
        value = found->second;
        return true;
    };
    const auto resolve_base = [](std::uint16_t key) -> std::optional<std::uint32_t> {
        return key == 9 ? std::optional{ 0x80500000u } : std::nullopt;
    };

    const std::vector<std::uint8_t> program{
        0x06, 0x03,
        0x03, 0x10, 0x00, 0x00, 0x00,
        0x02,
        0x08, 0x04, 0x20, 0x00, 0x00, 0x00,
        0x03, 0x04, 0x00, 0x00, 0x00,
        0x00,
    };
    RawAddressTrace trace;
    const auto result = evaluate_address_program_fixed(
        program, std::span<const std::uint32_t, 32>(gprs), true,
        resolve_base, read_guest, &trace);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x80400044u);
    ASSERT_TRUE(trace.success);
    ASSERT_EQ(trace.count, 6u);
    EXPECT_EQ(trace.operations[0].operation, 0x06u);
    EXPECT_EQ(trace.operations[1].address_after, 0x80300010u);
    EXPECT_TRUE(trace.operations[2].has_dereferenced_value);
    EXPECT_EQ(trace.operations[2].dereferenced_value, 0x80400000u);
    EXPECT_EQ(trace.operations[3].address_after, 0x80400040u);
    EXPECT_EQ(trace.operations[4].address_after, 0x80400044u);
    EXPECT_EQ(trace.operations[5].operation, 0x00u);

    const std::vector<std::uint8_t> base_program{
        0x01, 0x09, 0x00,
        0x03, 0x20, 0x00, 0x00, 0x00,
        0x00,
    };
    const auto base_result = evaluate_address_program_fixed(
        base_program, std::span<const std::uint32_t, 32>(gprs), true,
        resolve_base, read_guest, &trace);
    ASSERT_TRUE(base_result.has_value());
    EXPECT_EQ(*base_result, 0x80500020u);
    EXPECT_TRUE(trace.success);

    const std::vector<std::uint8_t> static_program{
        0x07, 0x78, 0x56, 0x34, 0x12,
        0x00,
    };
    const auto static_result = evaluate_address_program_fixed(
        static_program, std::span<const std::uint32_t, 32>(gprs), true,
        resolve_base, read_guest, &trace);
    ASSERT_TRUE(static_result.has_value());
    EXPECT_EQ(*static_result, 0x12345678u);
    EXPECT_TRUE(trace.success);
}

TEST(SavorProbeAddressProgram, ReportsTheExactFailingOperation)
{
    std::array<std::uint32_t, 32> gprs{};
    gprs[3] = 0x80300000u;
    const auto no_base = [](std::uint16_t) -> std::optional<std::uint32_t> {
        return std::nullopt;
    };
    const auto no_memory = [](
        std::uint32_t, SampleWidth, std::uint64_t&) { return false; };

    const std::vector<std::uint8_t> pointer_failure{ 0x06, 0x03, 0x02, 0x00 };
    RawAddressTrace trace;
    EXPECT_FALSE(evaluate_address_program_fixed(
        pointer_failure, std::span<const std::uint32_t, 32>(gprs), false,
        no_base, no_memory, &trace).has_value());
    EXPECT_FALSE(trace.success);
    EXPECT_EQ(trace.failure_operation, 1u);
    EXPECT_EQ(trace.failure, AddressProgramFailure::GuestReadFailed);
    ASSERT_EQ(trace.count, 2u);
    EXPECT_EQ(trace.operations[1].operation, 0x02u);
    EXPECT_EQ(trace.operations[1].failure, AddressProgramFailure::GuestReadFailed);
    EXPECT_EQ(trace.final_address, 0x80300000u);

    const std::vector<std::uint8_t> base_failure{ 0x01, 0x09, 0x00, 0x00 };
    EXPECT_FALSE(evaluate_address_program_fixed(
        base_failure, std::span<const std::uint32_t, 32>(gprs), true,
        no_base, no_memory, &trace).has_value());
    EXPECT_EQ(trace.failure_operation, 0u);
    EXPECT_EQ(trace.failure, AddressProgramFailure::BaseResolutionFailed);
}

TEST(SavorProbeProfile, RejectsUnboundedOrMalformedProfiles)
{
    std::string predicate;
    for (int i = 0; i < 65; ++i) {
        if (!predicate.empty())
            predicate += ',';
        predicate += R"({"op":"pc"})";
    }
    const std::string json = R"({
      "schema":"savor.capture.profile/1",
      "name":"invalid",
      "expected_module_sha256":"not-a-hash",
      "probes":[
        {"id":"bad","kind":"memory","address":1,"size":3,
         "subscriptions":["capture"],"predicate":[)"
        + predicate
        + R"(]},
        {"id":"bad","kind":"pc","address":2,"subscriptions":["capture"]}
      ]
    })";

    const auto result = parse_profile_json(json);
    EXPECT_FALSE(result.profile.has_value());
    const auto errors = format_profile_errors(result);
    EXPECT_NE(errors.find("64 operations"), std::string::npos);
    EXPECT_NE(errors.find("memory size"), std::string::npos);
    EXPECT_NE(errors.find("64 hex characters"), std::string::npos);
    EXPECT_NE(errors.find("duplicate probe id"), std::string::npos);
}

TEST(SavorProbeQueue, IsBoundedAndPreservesFifoOrder)
{
    BoundedEventQueue queue;
    ASSERT_TRUE(queue.configure(2));
    RawProbeEvent first{};
    first.capture_sequence = 1;
    RawProbeEvent second{};
    second.capture_sequence = 2;
    RawProbeEvent third{};
    third.capture_sequence = 3;
    EXPECT_TRUE(queue.try_push(first));
    EXPECT_TRUE(queue.try_push(second));
    EXPECT_FALSE(queue.try_push(third));
    EXPECT_EQ(queue.approximate_size(), 2u);

    RawProbeEvent result{};
    ASSERT_TRUE(queue.try_pop(result));
    EXPECT_EQ(result.capture_sequence, 1u);
    ASSERT_TRUE(queue.try_pop(result));
    EXPECT_EQ(result.capture_sequence, 2u);
    EXPECT_FALSE(queue.try_pop(result));
}

TEST(SavorProbeQueue, AcceptsConcurrentProducersWithoutCorruptingRecords)
{
    constexpr std::uint32_t kProducerCount = 4;
    constexpr std::uint32_t kEventsPerProducer = 16;
    BoundedEventQueue queue;
    ASSERT_TRUE(queue.configure(128));
    std::atomic<bool> failed{ false };
    std::vector<std::thread> producers;
    for (std::uint32_t producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([producer, &queue, &failed] {
            for (std::uint32_t index = 0; index < kEventsPerProducer; ++index) {
                RawProbeEvent event{};
                event.capture_sequence = producer * 1000 + index;
                event.pc = 0x80000000u + producer;
                event.value = index;
                if (!queue.try_push(event))
                    failed.store(true, std::memory_order_relaxed);
            }
        });
    }
    for (auto& producer : producers)
        producer.join();
    ASSERT_FALSE(failed.load(std::memory_order_relaxed));

    std::unordered_set<std::uint64_t> sequences;
    RawProbeEvent event{};
    while (queue.try_pop(event)) {
        EXPECT_EQ(event.pc, 0x80000000u + static_cast<std::uint32_t>(event.capture_sequence / 1000));
        EXPECT_EQ(event.value, event.capture_sequence % 1000);
        sequences.insert(event.capture_sequence);
    }
    EXPECT_EQ(sequences.size(), kProducerCount * kEventsPerProducer);
}

TEST(SavorProbeQueue, CoalescesToTheNewestProgressEventWithoutTornRecords)
{
    LatestRawProbeEvent latest;
    ASSERT_TRUE(latest.configure());
    constexpr std::uint64_t kEventCount = 2000;
    std::atomic<bool> producer_done{ false };
    std::atomic<bool> torn{ false };
    std::vector<std::uint64_t> observed;
    std::thread producer([&] {
        for (std::uint64_t sequence = 1; sequence <= kEventCount; ++sequence) {
            RawProbeEvent event{};
            event.capture_sequence = sequence;
            event.value = sequence * 3;
            event.pc = 0x80000000u + static_cast<std::uint32_t>(sequence % 0x10000u);
            bool replaced = false;
            if (!latest.publish(event, &replaced))
                torn.store(true, std::memory_order_relaxed);
        }
        producer_done.store(true, std::memory_order_release);
    });
    while (!producer_done.load(std::memory_order_acquire) || latest.has_pending()) {
        RawProbeEvent event{};
        if (!latest.try_take(event)) {
            std::this_thread::yield();
            continue;
        }
        if (event.value != event.capture_sequence * 3
            || event.pc != 0x80000000u + static_cast<std::uint32_t>(event.capture_sequence % 0x10000u)) {
            torn.store(true, std::memory_order_relaxed);
        }
        observed.push_back(event.capture_sequence);
    }
    producer.join();
    ASSERT_FALSE(torn.load(std::memory_order_relaxed));
    ASSERT_FALSE(observed.empty());
    EXPECT_EQ(observed.back(), kEventCount);
    EXPECT_TRUE(std::ranges::is_sorted(observed));
}

TEST(SavorProbeRetention, RetainsOnlyTheFinalAcceptedEventsPerLastNProbe)
{
    Profile profile;
    profile.probes.resize(3);
    profile.probes[0].id = "last-a";
    profile.probes[0].sampling = SamplingPolicy{ .mode = SamplingMode::LastN, .n = 2 };
    profile.probes[1].id = "immediate";
    profile.probes[2].id = "last-b";
    profile.probes[2].sampling = SamplingPolicy{ .mode = SamplingMode::LastN, .n = 1 };
    CaptureRetention retention(profile);
    std::vector<std::uint64_t> emitted;
    const auto emit = [&emitted](const RawProbeEvent& event) {
        emitted.push_back(event.capture_sequence);
    };
    const auto accept = [&](std::uint32_t probe, std::uint64_t sequence) {
        RawProbeEvent event{};
        event.probe_index = probe;
        event.capture_sequence = sequence;
        retention.accept(event, emit);
    };
    accept(0, 1);
    accept(1, 2);
    accept(2, 3);
    accept(0, 4);
    accept(2, 5);
    accept(0, 6);
    ASSERT_EQ(emitted, std::vector<std::uint64_t>{ 2 });
    retention.finish(emit);
    EXPECT_EQ(emitted, (std::vector<std::uint64_t>{ 2, 4, 5, 6 }));
}

TEST(SavorProbeRetention, CapturesFlightPrefixAndExtendsRetriggeredPostWindow)
{
    Profile profile;
    profile.probes.resize(3);
    profile.probes[0].id = "member";
    profile.probes[1].id = "trigger";
    profile.probes[2].id = "other";
    profile.flight_recorders.push_back(FlightRecorderDefinition{
        .id = "window",
        .member_probes = { "member" },
        .pre_events = 2,
        .post_events = 2,
        .trigger_probes = { "trigger" },
    });
    CaptureRetention retention(profile);
    std::vector<std::uint64_t> emitted;
    const auto emit = [&emitted](const RawProbeEvent& event) {
        emitted.push_back(event.capture_sequence);
    };
    const auto accept = [&](std::uint32_t probe, std::uint64_t sequence) {
        RawProbeEvent event{};
        event.probe_index = probe;
        event.capture_sequence = sequence;
        retention.accept(event, emit);
    };
    accept(0, 1);
    accept(0, 2);
    accept(0, 3);
    accept(1, 4);
    accept(0, 5);
    accept(1, 6);
    accept(0, 7);
    accept(0, 8);
    accept(0, 9);
    EXPECT_EQ(emitted, (std::vector<std::uint64_t>{ 2, 3, 4, 5, 6, 7, 8 }));
    const auto metrics = retention.flight_metrics();
    ASSERT_EQ(metrics.size(), 1u);
    EXPECT_EQ(metrics[0].triggers, 2u);
    EXPECT_EQ(metrics[0].retriggers, 1u);
    EXPECT_EQ(metrics[0].completed_windows, 1u);
    EXPECT_EQ(metrics[0].post_remaining, 0u);
    EXPECT_FALSE(metrics[0].active);
}

TEST(SavorProbeRetention, ActivatesOnlyFlightRecordersWithMatchingControlOrMarkerTriggers)
{
    Profile profile;
    profile.probes.resize(2);
    profile.probes[0].id = "control-member";
    profile.probes[1].id = "marker-member";
    profile.flight_recorders = {
        FlightRecorderDefinition{
            .id = "control",
            .member_probes = { "control-member" },
            .pre_events = 1,
            .post_events = 1,
            .trigger_on_control = true,
        },
        FlightRecorderDefinition{
            .id = "marker",
            .member_probes = { "marker-member" },
            .pre_events = 1,
            .post_events = 1,
            .trigger_markers = { "open.marker" },
        },
    };
    CaptureRetention retention(profile);
    std::vector<std::uint64_t> emitted;
    const auto emit = [&emitted](const RawProbeEvent& event) {
        emitted.push_back(event.capture_sequence);
    };
    RawProbeEvent control_pre{};
    control_pre.probe_index = 0;
    control_pre.capture_sequence = 1;
    retention.accept(control_pre, emit);
    RawProbeEvent marker_pre{};
    marker_pre.probe_index = 1;
    marker_pre.capture_sequence = 2;
    retention.accept(marker_pre, emit);

    RawProbeEvent control{};
    control.probe_index = kSyntheticControlProbeIndex;
    control.capture_sequence = 3;
    control.flags = kRawEventControlPublished;
    retention.accept(control, emit);
    RawProbeEvent control_post{};
    control_post.probe_index = 0;
    control_post.capture_sequence = 4;
    retention.accept(control_post, emit);

    RawProbeEvent marker{};
    marker.probe_index = kSyntheticMarkerProbeIndex;
    marker.capture_sequence = 5;
    constexpr std::string_view marker_id = "open.marker";
    marker.payload_size = static_cast<std::uint16_t>(marker_id.size());
    std::copy(marker_id.begin(), marker_id.end(), marker.payload.begin());
    retention.accept(marker, emit);
    RawProbeEvent marker_post{};
    marker_post.probe_index = 1;
    marker_post.capture_sequence = 6;
    retention.accept(marker_post, emit);

    EXPECT_EQ(emitted, (std::vector<std::uint64_t>{ 1, 3, 4, 2, 5, 6 }));
    const auto metrics = retention.flight_metrics();
    ASSERT_EQ(metrics.size(), 2u);
    EXPECT_EQ(metrics[0].triggers, 1u);
    EXPECT_EQ(metrics[1].triggers, 1u);
    EXPECT_EQ(metrics[0].completed_windows, 1u);
    EXPECT_EQ(metrics[1].completed_windows, 1u);
}

TEST(SavorProbeWindow, IncludesProbeOpenersClosersAndAcceptedEventLimit)
{
    WindowDefinition definition;
    definition.id = "turn";
    definition.open_probe = "open";
    definition.close_probe = "close";
    definition.event_limit = 3;
    ProbeWindowState state;
    state.reset(false);

    state.before_probe(definition, "open", 10);
    EXPECT_TRUE(state.is_open());
    state.after_probe(definition, "open", true, true, false, 10);
    EXPECT_TRUE(state.is_open());
    EXPECT_EQ(state.accepted_events(), 1u);

    state.before_probe(definition, "member", 10);
    state.after_probe(definition, "member", true, false, false, 10);
    EXPECT_EQ(state.accepted_events(), 1u);
    EXPECT_TRUE(state.is_open());
    state.after_probe(definition, "member", true, true, false, 10);
    EXPECT_TRUE(state.is_open());
    state.after_probe(definition, "close", true, true, false, 10);
    EXPECT_FALSE(state.is_open());
    EXPECT_EQ(state.accepted_events(), 3u);
}

TEST(SavorProbeWindow, AppliesMarkerControlAndFrameClosuresAfterTheirBoundaryEvent)
{
    WindowDefinition definition;
    definition.id = "window";
    definition.open_marker = "begin";
    definition.close_marker = "end";
    definition.open_on_control = true;
    definition.close_frame = 7;
    ProbeWindowState state;
    state.reset(false);

    state.on_marker(definition, "begin", 2);
    EXPECT_TRUE(state.is_open());
    state.on_marker(definition, "end", 4);
    EXPECT_FALSE(state.is_open());
    state.on_control(definition, 5);
    EXPECT_TRUE(state.is_open());
    state.on_frame(definition, 7);
    EXPECT_TRUE(state.is_open());
    state.after_probe(definition, "frame.end", true, true, true, 7);
    EXPECT_FALSE(state.is_open());
}

TEST(SavorProbeDispatch, RequiresAnActiveForegroundWakeAndIsolatesCaptureOverflow)
{
    const auto subscriptions = Subscription::Capture
        | Subscription::Progress | Subscription::Control;
    const auto shared = subscriber_dispatch_decision(
        subscriptions, true, true, false);
    EXPECT_TRUE(shared.capture);
    EXPECT_TRUE(shared.progress);
    EXPECT_TRUE(shared.control);

    const auto foreign_site = subscriber_dispatch_decision(
        subscriptions, true, false, false);
    EXPECT_TRUE(foreign_site.capture);
    EXPECT_TRUE(foreign_site.progress);
    EXPECT_FALSE(foreign_site.control);

    const auto after_capture_overflow = subscriber_dispatch_decision(
        subscriptions, false, true, false);
    EXPECT_FALSE(after_capture_overflow.capture);
    EXPECT_TRUE(after_capture_overflow.progress);
    EXPECT_TRUE(after_capture_overflow.control);

    const auto duplicate_control = subscriber_dispatch_decision(
        subscriptions, false, true, true);
    EXPECT_FALSE(duplicate_control.control);
}

TEST(SavorProbePolicy, RejectsReservedPcProbeAndActivationGate)
{
    constexpr std::uint32_t reserved_pc = 0x8007c7e4u;
    const std::array denied{ reserved_pc };

    Profile profile;
    profile.probes.push_back(ProbeDefinition{
        .id = "reserved-pc",
        .kind = ProbeKind::Pc,
        .address = reserved_pc,
    });
    std::string error;
    EXPECT_FALSE(ValidateProfilePcAccess(profile, denied, &error));
    EXPECT_EQ(error, "capture profile references a reserved runtime breakpoint");

    profile.probes = { ProbeDefinition{
        .id = "reserved-activation",
        .kind = ProbeKind::Memory,
        .address = 0x80400000u,
        .size = 4u,
        .activate_on_pc = reserved_pc,
    } };
    error.clear();
    EXPECT_FALSE(ValidateProfilePcAccess(profile, denied, &error));
    EXPECT_EQ(error, "capture profile references a reserved runtime breakpoint");
}

TEST(SavorProbePolicy, AllowsUnreservedProfilePcs)
{
    constexpr std::uint32_t reserved_pc = 0x8007c7e4u;
    const std::array denied{ reserved_pc };
    Profile profile;
    profile.probes = {
        ProbeDefinition{
            .id = "public-pc",
            .kind = ProbeKind::Pc,
            .address = 0x80001234u,
        },
        ProbeDefinition{
            .id = "public-activation",
            .kind = ProbeKind::Memory,
            .address = 0x80400000u,
            .size = 4u,
            .activate_on_pc = 0x80005678u,
        },
    };
    std::string error;
    EXPECT_TRUE(ValidateProfilePcAccess(profile, denied, &error));
    EXPECT_TRUE(error.empty());
}

} // namespace
