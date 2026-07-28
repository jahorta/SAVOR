#include <gtest/gtest.h>

#include <ActionViewStdJsonCache.h>
#include <BattlePredictor.h>
#include <CliResourceInputCompatibility.h>
#include <SpiceBattleContentAdapter.h>
#include <SpiceBattleContentAdapterTestSeam.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using namespace savor::predict;

std::string adapter_diagnostics(
    const SpiceBattleContentAdapterResult& result) {
    std::ostringstream out;
    for (const auto& diagnostic : result.diagnostics) {
        out << diagnostic.logical_role << ": " << diagnostic.message << '\n';
    }
    return out.str();
}

std::string canonical_std0_table(const Std0Table& table) {
    std::ostringstream out;
    out << "sentinel=" << table.includes_sentinel << ';';
    for (const auto& entry : table.entries) {
        out << entry.location_code << ','
            << entry.opcode << ','
            << entry.payload.primary_action_key << ','
            << entry.payload.generic_secondary_key << ','
            << entry.payload.direct_gate_secondary_key << ','
            << entry.has_payload << ';';
    }
    return out.str();
}

std::string canonical_action_rows(
    const std::vector<CombatantStdActionRow>& rows) {
    std::ostringstream out;
    for (const auto& row : rows) {
        out << row.index << ','
            << row.action_id << ','
            << row.row_type << ','
            << row.callback_index << ','
            << row.callback_ordinal << ','
            << row.flags << ','
            << row.secondary_key << ','
            << row.callback_aux_param << ','
            << row.transition_gate_divisor_bits << ','
            << row.motion_progress_step_bits << ';';
    }
    return out.str();
}

std::string canonical_visual_resource(
    const CombatantVisualResource& resource) {
    std::ostringstream out;
    out << "binding=" << resource.binding.resource_stem
        << ";sentinel=" << resource.includes_sentinel << ';';
    out << "rows=" << canonical_action_rows(resource.action_rows);
    out << "selector=" << canonical_std0_table(resource.selector_table);
    out << "motion=";
    for (const auto& motion : resource.motion_frame_counts) {
        out << motion.motion_id << ',' << motion.frame_count << ';';
    }
    out << "records=";
    for (const auto& record : resource.records) {
        out << record.index << ','
            << record.location_code << ','
            << record.opcode << ','
            << record.combined_type << ','
            << record.payload_size << ','
            << record.payload_in_bounds << ','
            << record.gate_fields_known << ','
            << record.gate_fields.primary_action_key << ','
            << record.gate_fields.generic_secondary_key << ','
            << record.gate_fields.direct_gate_secondary_key << ','
            << record.synchronization_gate << ','
            << static_cast<int>(record.kind) << ',';
        for (const auto byte : record.payload_bytes) {
            out << static_cast<unsigned int>(byte) << '.';
        }
        if (record.sparc.has_value()) {
            out << "sparc(" << record.sparc->source_key << ','
                << record.sparc->secondary_field << ')';
        }
        if (record.set_command.has_value()) {
            const auto& value = *record.set_command;
            out << "set(" << value.command_mode << ','
                << value.command_subtype << ','
                << value.synchronization_flags << ','
                << value.service_flags << ','
                << value.delay << ','
                << value.forced_mode << ')';
        }
        if (record.collision_box.has_value()) {
            const auto& value = *record.collision_box;
            out << "collision(" << value.behavior_flags << ','
                << value.start_counter << ','
                << value.end_counter << ','
                << value.object_id << ','
                << value.current_x_bits << ','
                << value.current_y_bits << ','
                << value.current_z_bits << ','
                << value.velocity_x_bits << ','
                << value.velocity_y_bits << ','
                << value.velocity_z_bits << ','
                << value.trailing_flags << ')';
        }
        if (record.system_camera.has_value()) {
            const auto& value = *record.system_camera;
            out << "camera(" << value.flags << ','
                << value.scalar_bits << ','
                << value.start_frame << ','
                << value.end_frame << ','
                << value.hold_frames << ','
                << value.step_frames << ','
                << value.mode << ')';
        }
        if (record.se_request.has_value()) {
            const auto& value = *record.se_request;
            out << "se(" << value.request_flags << ','
                << value.reserved_14 << ','
                << value.reserved_16 << ','
                << value.subtype << ','
                << value.trigger_frame << ','
                << value.end_frame << ','
                << value.channel << ','
                << value.candidate_a << ','
                << value.cue << ','
                << value.candidate_b << ','
                << value.candidate_c << ','
                << value.trailing_28 << ','
                << value.trailing_2a << ')';
        }
        out << ';';
    }
    return out.str();
}

const BattlePredictorMldMotionSlot* find_motion_slot(
    const BattlePredictorResourceTemplate& resource,
    std::size_t motion_slot) {
    for (const auto& entry : resource.mld_entries) {
        const auto found = std::find_if(
            entry.motion_slots.begin(),
            entry.motion_slots.end(),
            [motion_slot](const BattlePredictorMldMotionSlot& slot) {
                return slot.motion_slot == motion_slot;
            });
        if (found != entry.motion_slots.end()) {
            return &*found;
        }
    }
    return nullptr;
}

class ScopedAdapterTestTree {
public:
    explicit ScopedAdapterTestTree(std::string_view label) {
        const auto suffix = std::chrono::high_resolution_clock::now()
            .time_since_epoch()
            .count();
        path = std::filesystem::temp_directory_path()
            / ("savor_spice_adapter_" + std::string(label) + "_"
                + std::to_string(suffix));
    }

    ~ScopedAdapterTestTree() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::filesystem::path path;
};

void copy_bundle_sources(
    const BattlePredictorResourceBundle& source_bundle,
    const std::filesystem::path& target_root) {
    for (const auto& source : source_bundle.sources) {
        const auto target = target_root / source.relative_path;
        std::filesystem::create_directories(target.parent_path());
        std::filesystem::copy_file(
            source.source_path,
            target,
            std::filesystem::copy_options::overwrite_existing);
    }
}

TEST(
    SpiceBattleContentAdapter,
    DirectFirstBattleBundleUsesCompleteDeclaredMldCatalogs) {
    const auto root = default_action_view_std_disc_dump_root();
    if (!std::filesystem::is_directory(root)) {
        GTEST_SKIP() << "canonical loose disc dump is unavailable";
    }

    const auto loaded = load_first_battle_spice_resource_bundle({
        .disc_dump_root = root,
    });
    ASSERT_TRUE(loaded.ok()) << adapter_diagnostics(loaded);
    ASSERT_NE(loaded.bundle, nullptr);
    EXPECT_EQ(
        loaded.bundle->provider_kind,
        BattlePredictorResourceProviderKind::DirectSpice);
    EXPECT_EQ(loaded.bundle->sources.size(), 10U);
    EXPECT_EQ(loaded.bundle->resource_templates.size(), 3U);
    EXPECT_TRUE(
        loaded.bundle->target_reaction_effect_resource.has_value());
    EXPECT_FALSE(loaded.bundle->bundle_digest.empty());

    const std::map<std::string, std::size_t> expected_nonzero{
        {"ma000", 74U},
        {"ma001", 73U},
        {"mb000", 22U},
    };
    const std::map<std::string, std::uint32_t> expected_slot5{
        {"ma000", 30U},
        {"ma001", 30U},
        {"mb000", 31U},
    };

    std::size_t total_zero_slots = 0;
    std::size_t total_nonzero_slots = 0;
    std::set<std::uint32_t> unique_nonzero_addresses;
    for (const auto& [stem, expected_count] : expected_nonzero) {
        const auto* resource = loaded.bundle->find_resource(stem);
        ASSERT_NE(resource, nullptr) << stem;
        EXPECT_EQ(
            resource->status,
            BattlePredictorResourceInputStatus::Ready);
        std::size_t nonzero = 0;
        std::size_t decoded = 0;
        for (const auto& entry : resource->mld_entries) {
            EXPECT_TRUE(entry.complete);
            EXPECT_EQ(
                entry.declared_nonzero_motion_slots,
                entry.decoded_nonzero_motion_slots);
            for (const auto& slot : entry.motion_slots) {
                if (!slot.has_motion_address) {
                    ++total_zero_slots;
                    EXPECT_EQ(slot.source_motion_address, 0U);
                    EXPECT_FALSE(slot.decoded);
                    EXPECT_FALSE(slot.declared_frame_count.has_value());
                    continue;
                }
                ++nonzero;
                ++total_nonzero_slots;
                unique_nonzero_addresses.insert(
                    slot.source_motion_address);
                if (slot.decoded) {
                    ++decoded;
                }
                ASSERT_TRUE(slot.declared_frame_count.has_value());
            }
        }
        EXPECT_EQ(nonzero, expected_count) << stem;
        EXPECT_EQ(decoded, expected_count) << stem;
        EXPECT_EQ(
            resource->visual_resource.motion_frame_counts.size(),
            expected_count)
            << stem;
        const auto* slot5 = find_motion_slot(*resource, 5U);
        ASSERT_NE(slot5, nullptr) << stem;
        ASSERT_TRUE(slot5->declared_frame_count.has_value()) << stem;
        EXPECT_EQ(
            *slot5->declared_frame_count,
            expected_slot5.at(stem))
            << stem;
    }
    // The canonical three-file corpus currently has no empty slots, but the
    // zero-inclusive catalog contract is still exercised by synthetic adapter
    // projection tests.
    EXPECT_EQ(total_zero_slots, 0U);
    EXPECT_EQ(total_nonzero_slots, 169U);
    EXPECT_LT(unique_nonzero_addresses.size(), total_nonzero_slots);

    const auto* ma001 = loaded.bundle->find_resource("MA001");
    ASSERT_NE(ma001, nullptr);
    const auto* slot21 = find_motion_slot(*ma001, 21U);
    ASSERT_NE(slot21, nullptr);
    ASSERT_TRUE(slot21->declared_frame_count.has_value());
    EXPECT_EQ(*slot21->declared_frame_count, 81U);

    const auto* ma000 = loaded.bundle->find_resource("ma000");
    ASSERT_NE(ma000, nullptr);
    ASSERT_FALSE(ma000->visual_resource.action_rows.empty());
    const auto& first_action_row =
        ma000->visual_resource.action_rows.front();
    EXPECT_EQ(first_action_row.index, 0);
    EXPECT_EQ(first_action_row.action_id, 2);
    EXPECT_EQ(first_action_row.row_type, 1);
    EXPECT_EQ(first_action_row.callback_index, 8);
    EXPECT_EQ(first_action_row.callback_ordinal, 0);
    EXPECT_EQ(first_action_row.flags, 0x90b00000U);
    EXPECT_EQ(first_action_row.secondary_key, -1);
    EXPECT_EQ(first_action_row.callback_aux_param, 0);
    EXPECT_EQ(
        first_action_row.transition_gate_divisor_bits,
        0x40a00000U);
    EXPECT_EQ(
        first_action_row.motion_progress_step_bits,
        0x3f800000U);
    EXPECT_TRUE(ma000->visual_resource.includes_sentinel);
    EXPECT_TRUE(ma000->companion_std0_table.includes_sentinel);
    EXPECT_TRUE(
        ma000->runtime_aux_table_has_action_row_prefix);
    EXPECT_EQ(ma000->runtime_aux_table_prefix_rows, 1);
    ASSERT_FALSE(ma000->runtime_aux_table.entries.empty());
    EXPECT_EQ(
        ma000->runtime_aux_table.entries.front().payload
            .primary_action_key,
        first_action_row.action_id);
    EXPECT_EQ(
        ma000->runtime_aux_table.entries.front().payload
            .generic_secondary_key,
        first_action_row.row_type);
    EXPECT_EQ(
        ma000->runtime_aux_table.entries.front().payload
            .direct_gate_secondary_key,
        first_action_row.callback_index);

    const auto uppercase_primary = std::find_if(
        loaded.bundle->sources.begin(),
        loaded.bundle->sources.end(),
        [](const BattlePredictorResourceSourceIdentity& source) {
            return source.logical_role == "ma001.primary_std";
        });
    ASSERT_NE(uppercase_primary, loaded.bundle->sources.end());
    EXPECT_NE(
        uppercase_primary->relative_path.find("MA001.std"),
        std::string::npos);
    EXPECT_NE(
        uppercase_primary->normalized_relative_path.find("ma001.std"),
        std::string::npos);
}

TEST(
    SpiceBattleContentAdapter,
    DirectAndExplicitLegacyStdProvidersProjectAllSevenResourcesEqually) {
    const auto disc_root = default_action_view_std_disc_dump_root();
    const auto legacy_root =
        default_action_view_std_json_cache_dir("D:/SavorPredictDB");
    if (!std::filesystem::is_directory(disc_root)
        || !std::filesystem::is_directory(legacy_root)) {
        GTEST_SKIP() << "canonical direct or legacy corpus is unavailable";
    }

    const auto direct = load_first_battle_spice_resource_bundle({
        .disc_dump_root = disc_root,
    });
    const auto legacy = load_first_battle_spice_resource_bundle({
        .disc_dump_root = disc_root,
        .legacy_std_json_dir = legacy_root,
    });
    ASSERT_TRUE(direct.ok()) << adapter_diagnostics(direct);
    ASSERT_TRUE(legacy.ok()) << adapter_diagnostics(legacy);
    ASSERT_NE(direct.bundle, nullptr);
    ASSERT_NE(legacy.bundle, nullptr);
    EXPECT_EQ(
        legacy.bundle->provider_kind,
        BattlePredictorResourceProviderKind::LegacyStdJsonDirectMld);

    for (const auto stem : {"ma000", "ma001", "mb000"}) {
        const auto* direct_resource =
            direct.bundle->find_resource(stem);
        const auto* legacy_resource =
            legacy.bundle->find_resource(stem);
        ASSERT_NE(direct_resource, nullptr) << stem;
        ASSERT_NE(legacy_resource, nullptr) << stem;
        EXPECT_EQ(
            canonical_action_rows(
                direct_resource->visual_resource.action_rows),
            canonical_action_rows(
                legacy_resource->visual_resource.action_rows))
            << stem << ".std";
        EXPECT_EQ(
            canonical_visual_resource(
                direct_resource->visual_resource),
            canonical_visual_resource(
                legacy_resource->visual_resource))
            << stem << "0.std";
        EXPECT_EQ(
            canonical_std0_table(
                direct_resource->companion_std0_table),
            canonical_std0_table(
                legacy_resource->companion_std0_table))
            << stem << "0.std companion table";
        EXPECT_EQ(
            canonical_std0_table(direct_resource->runtime_aux_table),
            canonical_std0_table(legacy_resource->runtime_aux_table))
            << stem << " synthesized runtime table";
        EXPECT_EQ(
            direct_resource->runtime_aux_table_has_action_row_prefix,
            legacy_resource->runtime_aux_table_has_action_row_prefix);
        EXPECT_EQ(
            direct_resource->runtime_aux_table_prefix_rows,
            legacy_resource->runtime_aux_table_prefix_rows);
    }

    ASSERT_TRUE(
        direct.bundle->target_reaction_effect_resource.has_value());
    ASSERT_TRUE(
        legacy.bundle->target_reaction_effect_resource.has_value());
    EXPECT_EQ(
        canonical_visual_resource(
            *direct.bundle->target_reaction_effect_resource),
        canonical_visual_resource(
            *legacy.bundle->target_reaction_effect_resource))
        << "damage.std";
}

TEST(
    SpiceBattleContentAdapter,
    BundleIdentityDoesNotDependOnMachineLocalRootPath) {
    const auto source_root = default_action_view_std_disc_dump_root();
    if (!std::filesystem::is_directory(source_root)) {
        GTEST_SKIP() << "canonical loose disc dump is unavailable";
    }
    const auto source = load_first_battle_spice_resource_bundle({
        .disc_dump_root = source_root,
    });
    ASSERT_TRUE(source.ok()) << adapter_diagnostics(source);

    ScopedAdapterTestTree first("identity_a");
    ScopedAdapterTestTree second("identity_b");
    copy_bundle_sources(*source.bundle, first.path);
    copy_bundle_sources(*source.bundle, second.path);

    const auto loaded_first = load_first_battle_spice_resource_bundle({
        .disc_dump_root = first.path,
    });
    const auto loaded_second = load_first_battle_spice_resource_bundle({
        .disc_dump_root = second.path,
    });
    ASSERT_TRUE(loaded_first.ok()) << adapter_diagnostics(loaded_first);
    ASSERT_TRUE(loaded_second.ok()) << adapter_diagnostics(loaded_second);
    EXPECT_EQ(
        loaded_first.bundle->bundle_digest,
        loaded_second.bundle->bundle_digest);
    ASSERT_EQ(
        loaded_first.bundle->sources.size(),
        loaded_second.bundle->sources.size());
    for (std::size_t index = 0;
         index < loaded_first.bundle->sources.size();
         ++index) {
        const auto& lhs = loaded_first.bundle->sources[index];
        const auto& rhs = loaded_second.bundle->sources[index];
        EXPECT_EQ(lhs.logical_role, rhs.logical_role);
        EXPECT_EQ(
            lhs.normalized_relative_path,
            rhs.normalized_relative_path);
        EXPECT_EQ(lhs.size_bytes, rhs.size_bytes);
        EXPECT_EQ(lhs.sha256, rhs.sha256);
        EXPECT_NE(lhs.source_path, rhs.source_path);
    }
}

TEST(
    SpiceBattleContentAdapter,
    BundleIdentityDoesNotDependOnDiscRootVersusBcharaLocatorShape) {
    const auto source_root = default_action_view_std_disc_dump_root();
    if (!std::filesystem::is_directory(source_root)) {
        GTEST_SKIP() << "canonical loose disc dump is unavailable";
    }
    const auto source = load_first_battle_spice_resource_bundle({
        .disc_dump_root = source_root,
    });
    ASSERT_TRUE(source.ok()) << adapter_diagnostics(source);

    ScopedAdapterTestTree tree("identity_locator_shape");
    copy_bundle_sources(*source.bundle, tree.path);
    const auto from_disc_root = load_first_battle_spice_resource_bundle({
        .disc_dump_root = tree.path,
    });
    const auto from_bchara = load_first_battle_spice_resource_bundle({
        .disc_dump_root = tree.path / "bchara",
    });
    ASSERT_TRUE(from_disc_root.ok())
        << adapter_diagnostics(from_disc_root);
    ASSERT_TRUE(from_bchara.ok())
        << adapter_diagnostics(from_bchara);
    EXPECT_EQ(
        from_disc_root.bundle->bundle_digest,
        from_bchara.bundle->bundle_digest);
    ASSERT_EQ(
        from_disc_root.bundle->sources.size(),
        from_bchara.bundle->sources.size());
    for (std::size_t index = 0;
         index < from_disc_root.bundle->sources.size();
         ++index) {
        const auto& lhs = from_disc_root.bundle->sources[index];
        const auto& rhs = from_bchara.bundle->sources[index];
        EXPECT_EQ(lhs.logical_role, rhs.logical_role);
        EXPECT_EQ(lhs.relative_path, rhs.relative_path);
        EXPECT_EQ(
            lhs.normalized_relative_path,
            rhs.normalized_relative_path);
        EXPECT_EQ(lhs.size_bytes, rhs.size_bytes);
        EXPECT_EQ(lhs.sha256, rhs.sha256);
        EXPECT_EQ(lhs.parser_identity, rhs.parser_identity);
    }
}

TEST(
    SpiceBattleContentAdapter,
    ReadyBundleIsReusedWithoutTouchingTheLocatorAgain) {
    const auto source_root = default_action_view_std_disc_dump_root();
    if (!std::filesystem::is_directory(source_root)) {
        GTEST_SKIP() << "canonical loose disc dump is unavailable";
    }
    const auto loaded = load_first_battle_spice_resource_bundle({
        .disc_dump_root = source_root,
    });
    ASSERT_TRUE(loaded.ok()) << adapter_diagnostics(loaded);

    ScopedAdapterTestTree missing("reuse_missing_locator");
    auto locator = missing.path;
    auto reused = loaded.bundle;
    std::ostringstream error;
    EXPECT_TRUE(cli_detail::ensure_first_battle_resource_inputs(
        locator,
        {},
        reused,
        error));
    EXPECT_EQ(reused, loaded.bundle);
    EXPECT_EQ(locator, missing.path);
    EXPECT_TRUE(error.str().empty());
}

TEST(
    SpiceBattleContentAdapter,
    MissingDiscRootIsAStickyMissingInputBundle) {
    ScopedAdapterTestTree missing("missing");
    const auto loaded = load_first_battle_spice_resource_bundle({
        .disc_dump_root = missing.path,
    });
    ASSERT_NE(loaded.bundle, nullptr);
    EXPECT_FALSE(loaded.ok());
    EXPECT_EQ(
        loaded.status,
        BattlePredictorResourceInputStatus::MissingInput);
    EXPECT_EQ(
        loaded.bundle->status,
        BattlePredictorResourceInputStatus::MissingInput);
    EXPECT_FALSE(loaded.diagnostics.empty());
}

TEST(
    SpiceBattleContentAdapter,
    PredictorSemanticContractRequiresTheImmutableBundle) {
    BattlePredictionInput missing;
    const auto missing_result = predict_battle(missing);
    EXPECT_EQ(
        missing_result.outcome,
        BattlePredictionOutcome::MissingInput);
    ASSERT_FALSE(missing_result.events.empty());
    const auto missing_event = std::find_if(
        missing_result.events.begin(),
        missing_result.events.end(),
        [](const BattlePredictionEvent& event) {
            return event.label == "resource_inputs_missing";
        });
    ASSERT_NE(missing_event, missing_result.events.end());
    EXPECT_EQ(
        missing_event->status,
        BattlePredictionEventStatus::MissingInput);

    auto ambiguous_bundle =
        std::make_shared<BattlePredictorResourceBundle>();
    ambiguous_bundle->status =
        BattlePredictorResourceInputStatus::Ambiguous;
    ambiguous_bundle->diagnostics.push_back({
        .severity = BattlePredictorResourceDiagnosticSeverity::Error,
        .logical_role = "ma000.primary_std",
        .message = "case-colliding test inputs",
    });
    BattlePredictionInput ambiguous;
    ambiguous.resource_inputs = ambiguous_bundle;
    const auto ambiguous_result = predict_battle(ambiguous);
    EXPECT_EQ(
        ambiguous_result.outcome,
        BattlePredictionOutcome::Ambiguous);
    const auto ambiguous_event = std::find_if(
        ambiguous_result.events.begin(),
        ambiguous_result.events.end(),
        [](const BattlePredictionEvent& event) {
            return event.label == "resource_inputs_ambiguous";
        });
    ASSERT_NE(ambiguous_event, ambiguous_result.events.end());
    EXPECT_EQ(
        ambiguous_event->status,
        BattlePredictionEventStatus::Ambiguous);
}

adapter_test::MldCatalogProjectionInput single_motion_catalog_input(
    std::uint32_t declared_frame_count = 12U) {
    return adapter_test::MldCatalogProjectionInput{
        .logical_role = "synthetic.mld",
        .entries = {
            adapter_test::MldCatalogEntryInput{
                .table_index = 0,
                .source_entry_id = 1,
                .motion_addresses = {0x100U},
                .declared_nonzero_motion_slots = 1,
            },
        },
        .resources = {
            adapter_test::MldCatalogResourceInput{
                .source_motion_address = 0x100U,
                .variants = {
                    adapter_test::MldCatalogVariantInput{
                        .node_count = 4,
                        .short_rotation = false,
                        .has_motion = true,
                        .declared_frame_count = declared_frame_count,
                    },
                },
            },
        },
        .bindings = {
            adapter_test::MldCatalogBindingInput{
                .table_index = 0,
                .source_entry_id = 1,
                .motion_slot = 0,
                .source_motion_address = 0x100U,
                .source_object_address = 0x80U,
                .node_count = 4,
                .short_rotation = false,
                .variant_index = 0,
            },
        },
    };
}

TEST(
    SpiceBattleContentAdapter,
    SyntheticCatalogPreservesZeroSlotsRepeatedAddressesAndDeclaredZero) {
    adapter_test::MldCatalogProjectionInput input{
        .logical_role = "synthetic.mld",
        .entries = {
            adapter_test::MldCatalogEntryInput{
                .table_index = 0,
                .source_entry_id = 7,
                .motion_addresses = {0U, 0x100U, 0x100U, 0x200U},
                .declared_nonzero_motion_slots = 3,
            },
        },
        .resources = {
            adapter_test::MldCatalogResourceInput{
                .source_motion_address = 0x100U,
                .variants = {
                    adapter_test::MldCatalogVariantInput{
                        .node_count = 4,
                        .has_motion = true,
                        .declared_frame_count = 12,
                    },
                },
            },
            adapter_test::MldCatalogResourceInput{
                .source_motion_address = 0x200U,
                .variants = {
                    adapter_test::MldCatalogVariantInput{
                        .node_count = 4,
                        .has_motion = true,
                        .declared_frame_count = 0,
                    },
                },
            },
        },
        .bindings = {
            adapter_test::MldCatalogBindingInput{
                .table_index = 0,
                .source_entry_id = 7,
                .motion_slot = 1,
                .source_motion_address = 0x100U,
                .source_object_address = 0x80U,
                .node_count = 4,
                .variant_index = 0,
            },
            adapter_test::MldCatalogBindingInput{
                .table_index = 0,
                .source_entry_id = 7,
                .motion_slot = 2,
                .source_motion_address = 0x100U,
                .source_object_address = 0x80U,
                .node_count = 4,
                .variant_index = 0,
            },
            adapter_test::MldCatalogBindingInput{
                .table_index = 0,
                .source_entry_id = 7,
                .motion_slot = 3,
                .source_motion_address = 0x200U,
                .source_object_address = 0x80U,
                .node_count = 4,
                .variant_index = 0,
            },
        },
    };

    const auto result =
        adapter_test::project_mld_motion_catalog(input);
    ASSERT_EQ(
        result.status,
        BattlePredictorResourceInputStatus::Ready);
    ASSERT_EQ(result.entries.size(), 1U);
    const auto& entry = result.entries.front();
    EXPECT_TRUE(entry.complete);
    ASSERT_EQ(entry.motion_slots.size(), 4U);
    EXPECT_FALSE(entry.motion_slots[0].has_motion_address);
    EXPECT_FALSE(entry.motion_slots[0].decoded);
    EXPECT_FALSE(entry.motion_slots[0].declared_frame_count.has_value());
    EXPECT_TRUE(entry.motion_slots[1].decoded);
    EXPECT_TRUE(entry.motion_slots[2].decoded);
    EXPECT_EQ(
        entry.motion_slots[1].source_motion_address,
        entry.motion_slots[2].source_motion_address);
    ASSERT_TRUE(entry.motion_slots[3].declared_frame_count.has_value());
    EXPECT_EQ(*entry.motion_slots[3].declared_frame_count, 0U);
    ASSERT_EQ(result.motion_frame_counts.size(), 3U);
    EXPECT_EQ(result.motion_frame_counts[0].frame_count, 12U);
    EXPECT_EQ(result.motion_frame_counts[1].frame_count, 12U);
    EXPECT_EQ(result.motion_frame_counts[2].frame_count, 0U);
}

TEST(
    SpiceBattleContentAdapter,
    SyntheticCatalogClassifiesMissingBindingResourceAndVariantAsMissingInput) {
    {
        auto input = single_motion_catalog_input();
        input.entries.front().declared_nonzero_motion_slots = 2;
        EXPECT_EQ(
            adapter_test::project_mld_motion_catalog(input).status,
            BattlePredictorResourceInputStatus::MissingInput);
    }
    {
        auto input = single_motion_catalog_input();
        input.entries.front().has_motion_address_list = false;
        EXPECT_EQ(
            adapter_test::project_mld_motion_catalog(input).status,
            BattlePredictorResourceInputStatus::MissingInput);
    }
    {
        auto input = single_motion_catalog_input();
        input.bindings.clear();
        EXPECT_EQ(
            adapter_test::project_mld_motion_catalog(input).status,
            BattlePredictorResourceInputStatus::MissingInput);
    }
    {
        auto input = single_motion_catalog_input();
        input.resources.clear();
        EXPECT_EQ(
            adapter_test::project_mld_motion_catalog(input).status,
            BattlePredictorResourceInputStatus::MissingInput);
    }
    {
        auto input = single_motion_catalog_input();
        input.resources.front().variants.clear();
        EXPECT_EQ(
            adapter_test::project_mld_motion_catalog(input).status,
            BattlePredictorResourceInputStatus::MissingInput);
    }
    {
        auto input = single_motion_catalog_input();
        input.resources.front().variants.front().has_motion = false;
        EXPECT_EQ(
            adapter_test::project_mld_motion_catalog(input).status,
            BattlePredictorResourceInputStatus::MissingInput);
    }
    {
        auto input = single_motion_catalog_input();
        input.bindings.front().variant_index = 1;
        EXPECT_EQ(
            adapter_test::project_mld_motion_catalog(input).status,
            BattlePredictorResourceInputStatus::MissingInput);
    }
}

TEST(
    SpiceBattleContentAdapter,
    SyntheticCatalogClassifiesCompatibleVariantAndFrameCountConflictsAsAmbiguous) {
    {
        auto input = single_motion_catalog_input();
        input.bindings.push_back(input.bindings.front());
        EXPECT_EQ(
            adapter_test::project_mld_motion_catalog(input).status,
            BattlePredictorResourceInputStatus::Ambiguous);
    }
    {
        auto input = single_motion_catalog_input();
        input.resources.push_back(input.resources.front());
        EXPECT_EQ(
            adapter_test::project_mld_motion_catalog(input).status,
            BattlePredictorResourceInputStatus::Ambiguous);
    }
    {
        auto input = single_motion_catalog_input();
        input.resources.front().variants.push_back(
            input.resources.front().variants.front());
        EXPECT_EQ(
            adapter_test::project_mld_motion_catalog(input).status,
            BattlePredictorResourceInputStatus::Ambiguous);
    }
    {
        auto input = single_motion_catalog_input(10U);
        input.entries.push_back(adapter_test::MldCatalogEntryInput{
            .table_index = 1,
            .source_entry_id = 2,
            .motion_addresses = {0x200U},
            .declared_nonzero_motion_slots = 1,
        });
        input.resources.push_back(adapter_test::MldCatalogResourceInput{
            .source_motion_address = 0x200U,
            .variants = {
                adapter_test::MldCatalogVariantInput{
                    .node_count = 4,
                    .has_motion = true,
                    .declared_frame_count = 11U,
                },
            },
        });
        input.bindings.push_back(adapter_test::MldCatalogBindingInput{
            .table_index = 1,
            .source_entry_id = 2,
            .motion_slot = 0,
            .source_motion_address = 0x200U,
            .source_object_address = 0x90U,
            .node_count = 4,
            .variant_index = 0,
        });
        EXPECT_EQ(
            adapter_test::project_mld_motion_catalog(input).status,
            BattlePredictorResourceInputStatus::Ambiguous);
    }
}

TEST(
    SpiceBattleContentAdapter,
    SyntheticCatalogKeepsMissingInputStickyAcrossMixedStatusOrder) {
    {
        auto missing_then_ambiguous = single_motion_catalog_input();
        missing_then_ambiguous.entries.front()
            .declared_nonzero_motion_slots = 2;
        missing_then_ambiguous.bindings.push_back(
            missing_then_ambiguous.bindings.front());
        EXPECT_EQ(
            adapter_test::project_mld_motion_catalog(
                missing_then_ambiguous)
                .status,
            BattlePredictorResourceInputStatus::MissingInput);
    }
    {
        auto ambiguous_then_missing = single_motion_catalog_input();
        ambiguous_then_missing.bindings.push_back(
            ambiguous_then_missing.bindings.front());
        ambiguous_then_missing.entries.push_back(
            adapter_test::MldCatalogEntryInput{
                .table_index = 1,
                .source_entry_id = 2,
                .motion_addresses = {0x200U},
                .declared_nonzero_motion_slots = 1,
            });
        ambiguous_then_missing.resources.push_back(
            adapter_test::MldCatalogResourceInput{
                .source_motion_address = 0x200U,
                .variants = {
                    adapter_test::MldCatalogVariantInput{
                        .node_count = 4,
                        .has_motion = true,
                        .declared_frame_count = 13U,
                    },
                },
            });
        EXPECT_EQ(
            adapter_test::project_mld_motion_catalog(
                ambiguous_then_missing)
                .status,
            BattlePredictorResourceInputStatus::MissingInput);
    }
}

TEST(
    SpiceBattleContentAdapter,
    SyntheticCatalogKeepsUnrelatedWarningsReady) {
    auto input = single_motion_catalog_input();
    input.diagnostics.push_back(adapter_test::MldCatalogDiagnosticInput{
        .severity = BattlePredictorResourceDiagnosticSeverity::Warning,
        .message = "unrelated object-reader warning",
        .source_offset = 0x20U,
    });
    const auto result =
        adapter_test::project_mld_motion_catalog(input);
    EXPECT_EQ(
        result.status,
        BattlePredictorResourceInputStatus::Ready);
    ASSERT_EQ(result.diagnostics.size(), 1U);
    EXPECT_EQ(
        result.diagnostics.front().diagnostic.severity,
        BattlePredictorResourceDiagnosticSeverity::Warning);
}

TEST(
    SpiceBattleContentAdapter,
    CaseInsensitiveSelectionClassifiesCardinalityAndRetainsActualSpelling) {
    {
        const std::vector<std::string> candidates{
            "unrelated.std",
        };
        const auto selected =
            adapter_test::select_case_insensitive_candidate(
                "ma001.std",
                candidates);
        EXPECT_EQ(
            selected.status,
            BattlePredictorResourceInputStatus::MissingInput);
        EXPECT_FALSE(selected.selected_index.has_value());
    }
    {
        const std::vector<std::string> candidates{
            "unrelated.std",
            "MA001.std",
        };
        const auto selected =
            adapter_test::select_case_insensitive_candidate(
                "ma001.std",
                candidates);
        EXPECT_EQ(
            selected.status,
            BattlePredictorResourceInputStatus::Ready);
        ASSERT_TRUE(selected.selected_index.has_value());
        EXPECT_EQ(candidates[*selected.selected_index], "MA001.std");
    }
    {
        const std::vector<std::string> candidates{
            "ma001.std",
            "MA001.std",
        };
        const auto selected =
            adapter_test::select_case_insensitive_candidate(
                "ma001.std",
                candidates);
        EXPECT_EQ(
            selected.status,
            BattlePredictorResourceInputStatus::Ambiguous);
        EXPECT_FALSE(selected.selected_index.has_value());
    }
}

} // namespace
