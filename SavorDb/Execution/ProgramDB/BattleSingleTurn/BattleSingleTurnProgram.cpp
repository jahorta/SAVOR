#include "BattleSingleTurnProgram.h"
#include "../WorksetObservationBinding.h"
#include "../WorksetDerivedStateBinding.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../IExecutionDb.h"
#include "../../Workflow/WorkflowOrchestration.h"
#include "../../../Analysis/IAnalysisDb.h"
#include "../../../Authoring/IAuthoringDb.h"
#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../State/IStateDb.h"
#include "../../../../SavorCore/Core/Input/SoaBattle/BattleCommandCodec.h"
#include "../../../../SavorCore/Core/Memory/Soa/Battle/BattleContextCodec.h"
#include "../../../../SavorCore/Phases/Programs/BattleSingleTurn/BattleSingleTurnModule.h"
#include "../../../../SavorCore/Runner/IPC/DurableWorkerTerminalEnvelope.h"
#include "../../../../SavorCore/Runner/Runtime/ProgramKind.h"
#include "../../../../SavorCore/Runner/Runtime/Predicates/PredicateExecution.h"
#include "../../../../SavorCore/Runner/Runtime/DerivedState/DerivedStateRegistry.h"
#include "../../../../SavorCore/Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "../../../../SavorCore/Runner/Runtime/Worksets/WorksetWireCodec.h"
#include "../../../../SavorCore/Utils/Hash.h"

namespace savor::db::execution::programdb::battle {

bool ValidateBattlePlanTurnSequence(
    const BattlePlanSnapshot& plan,
    std::string* error_out) {
    if (plan.turns.empty()) {
        if (error_out) *error_out = "Battle Plan must contain turn 1";
        return false;
    }
    std::vector<int> indices;
    indices.reserve(plan.turns.size());
    for (const auto& turn : plan.turns) indices.push_back(turn.turn_index);
    std::ranges::sort(indices);
    for (std::size_t ordinal = 0; ordinal < indices.size(); ++ordinal) {
        const auto expected = static_cast<int>(ordinal) + 1;
        if (indices[ordinal] != expected) {
            if (error_out) *error_out = "Battle Plan turns must be positive, one-based, and contiguous; expected turn "
                + std::to_string(expected) + " but found "
                + std::to_string(indices[ordinal]);
            return false;
        }
    }
    return true;
}

BattleTargetAvailability ClassifyBattleTurnTargets(
    const soa::battle::ctx::BattleContext* context,
    const soa::battle::actions::BattleTurnCommandSet& commands) noexcept {
    if (context == nullptr) return BattleTargetAvailability::Unknown;
    for (const auto& command : commands) {
        const bool needs_target =
            command.macro == soa::battle::actions::BattleAction::Attack
            || command.macro == soa::battle::actions::BattleAction::UseItem;
        if (!needs_target) continue;
        if (command.params.target_slot >= soa::battle::ctx::SLOT_COUNT)
            return BattleTargetAvailability::Unavailable;
        const auto& slot = context->slots_[command.params.target_slot];
        if (!slot.present || slot.is_player || !slot.is_alive)
            return BattleTargetAvailability::Unavailable;
    }
    return BattleTargetAvailability::Available;
}

std::optional<BattleTurnVariantCompilation> CompileBattleTurnVariants(
    const BattlePlanTurnSnapshot& turn,
    const soa::battle::ctx::BattleContext* context,
    std::string* error_out) {
    using soa::battle::actions::BattleAction;
    using soa::battle::actions::BattleCommand;
    using soa::battle::actions::BattleTurnCommandSet;

    const auto fail = [&](std::string message)
        -> std::optional<BattleTurnVariantCompilation> {
        if (error_out) *error_out = std::move(message);
        return std::nullopt;
    };
    const auto needs_target = [](BattleAction action) {
        return action == BattleAction::Attack || action == BattleAction::UseItem;
    };

    struct PlannedAction {
        BattlePlanActionSnapshot source;
        BattleCommand base{};
        bool needs_target = false;
        std::vector<int> direct_domain;
        std::optional<int> same_as_actor;
    };

    std::vector<BattlePlanActionSnapshot> ordered = turn.actions;
    std::ranges::sort(ordered, {}, &BattlePlanActionSnapshot::ordinal);
    if (ordered.empty()) return fail("Battle Plan turn has no commands");

    std::set<int> actor_slots;
    std::set<int> direct_target_actors;
    std::map<int, int> same_as_by_actor;
    std::vector<PlannedAction> planned;
    planned.reserve(ordered.size());
    constexpr int kEnemyMask = 0x0ff0;

    for (std::size_t index = 0; index < ordered.size(); ++index) {
        const auto& action = ordered[index];
        if (action.ordinal != static_cast<int>(index))
            return fail("Battle Plan action ordinals must be zero-based, unique, and contiguous");
        if (action.actor_slot < 0 || action.actor_slot > 3
            || !actor_slots.insert(action.actor_slot).second) {
            return fail("Battle Plan actors must be unique slots 0 through 3");
        }
        const auto& preset = action.action_preset;
        if (soa::battle::actions::find_battle_action_definition(
                static_cast<std::int64_t>(preset.macro)) == nullptr) {
            return fail("Battle Plan action macro is unknown");
        }

        PlannedAction item{};
        item.source = action;
        item.base.actor_slot = static_cast<std::uint8_t>(action.actor_slot);
        item.base.macro = preset.macro;
        item.base.params.target_slot = 0xff;
        item.base.params.item_id = 0xffff;
        item.needs_target = needs_target(preset.macro);

        if (preset.macro == BattleAction::UseItem) {
            if (!preset.item_id || *preset.item_id < 0 || *preset.item_id > 0xffff)
                return fail("UseItem requires a concrete item identifier");
            item.base.params.item_id = static_cast<std::uint16_t>(*preset.item_id);
        }

        if (item.needs_target) {
            switch (preset.target_kind) {
            case BattlePlanTargetKind::SingleEnemy:
                if (!preset.target_single_slot
                    || *preset.target_single_slot < 4
                    || *preset.target_single_slot > 11
                    || preset.target_mask_bits
                    || preset.target_same_as_actor_slot) {
                    return fail("SingleEnemy requires exactly one enemy slot from 4 through 11");
                }
                item.direct_domain.push_back(*preset.target_single_slot);
                direct_target_actors.insert(action.actor_slot);
                break;
            case BattlePlanTargetKind::MultipleEnemies:
                if (!preset.target_mask_bits || *preset.target_mask_bits <= 0
                    || (*preset.target_mask_bits & ~kEnemyMask) != 0
                    || preset.target_single_slot
                    || preset.target_same_as_actor_slot) {
                    return fail("MultipleEnemies requires a nonempty mask containing only enemy slots 4 through 11");
                }
                for (int slot = 4; slot <= 11; ++slot) {
                    if ((*preset.target_mask_bits & (1 << slot)) != 0)
                        item.direct_domain.push_back(slot);
                }
                direct_target_actors.insert(action.actor_slot);
                break;
            case BattlePlanTargetKind::AnyEnemy:
                if (preset.target_mask_bits || preset.target_single_slot
                    || preset.target_same_as_actor_slot) {
                    return fail("AnyEnemy does not accept a target payload");
                }
                for (int slot = 4; slot <= 11; ++slot)
                    item.direct_domain.push_back(slot);
                direct_target_actors.insert(action.actor_slot);
                break;
            case BattlePlanTargetKind::SameAsOtherPC:
                if (!preset.target_same_as_actor_slot
                    || *preset.target_same_as_actor_slot < 0
                    || *preset.target_same_as_actor_slot > 3
                    || preset.target_mask_bits || preset.target_single_slot) {
                    return fail("SameAsOtherPC requires exactly one actor slot from 0 through 3");
                }
                item.same_as_actor = *preset.target_same_as_actor_slot;
                same_as_by_actor.emplace(action.actor_slot, *item.same_as_actor);
                break;
            default:
                return fail("Battle Plan target kind is unknown");
            }
        }
        planned.push_back(std::move(item));
    }

    for (const auto& [actor, referenced] : same_as_by_actor) {
        (void)actor;
        if (!actor_slots.contains(referenced))
            return fail("SameAsOtherPC references an actor that is not present in the turn");
    }
    const auto resolves_to_direct = [&](const auto& self, int actor,
                                        std::set<int>& visiting) -> bool {
        if (direct_target_actors.contains(actor)) return true;
        if (!visiting.insert(actor).second) return false;
        const auto same = same_as_by_actor.find(actor);
        if (same == same_as_by_actor.end()) return false;
        const bool resolved = self(self, same->second, visiting);
        visiting.erase(actor);
        return resolved;
    };
    for (const auto& [actor, referenced] : same_as_by_actor) {
        (void)referenced;
        std::set<int> visiting;
        if (!resolves_to_direct(resolves_to_direct, actor, visiting))
            return fail("SameAsOtherPC references must be acyclic and terminate at a direct target selector");
    }

    BattleTurnVariantCompilation compilation{};
    compilation.context_applied = context != nullptr;
    std::vector<BattleCommand> current(planned.size());
    std::map<int, int> direct_target_by_actor;
    std::set<std::string> encoded_seen;
    std::map<std::string, std::string> encoded_by_hash;

    const auto resolve_assigned_target = [&](const auto& self, int actor,
                                             std::set<int>& visiting)
        -> std::optional<int> {
        if (const auto direct = direct_target_by_actor.find(actor);
            direct != direct_target_by_actor.end()) return direct->second;
        if (!visiting.insert(actor).second) return std::nullopt;
        const auto same = same_as_by_actor.find(actor);
        if (same == same_as_by_actor.end()) return std::nullopt;
        const auto resolved = self(self, same->second, visiting);
        visiting.erase(actor);
        return resolved;
    };

    bool collision = false;
    const auto emit = [&]() {
        BattleTurnCommandSet commands;
        commands.reserve(planned.size());
        for (std::size_t index = 0; index < planned.size(); ++index) {
            auto command = current[index];
            if (planned[index].same_as_actor) {
                std::set<int> visiting;
                const auto target = resolve_assigned_target(
                    resolve_assigned_target,
                    planned[index].source.actor_slot,
                    visiting);
                if (!target) return;
                command.params.target_slot = static_cast<std::uint8_t>(*target);
            }
            commands.push_back(command);
        }
        std::vector<std::uint8_t> encoded_bytes;
        soa::battle::actions::encode_battle_turn_commands_to_buffer(
            commands, encoded_bytes);
        const auto encoded =
            soa::battle::actions::encode_battle_turn_commands_hex(commands);
        if (!encoded_seen.insert(encoded).second) return;
        const auto variant_key = hash::sha256(
            encoded_bytes.data(), encoded_bytes.size());
        const auto [where, inserted] = encoded_by_hash.emplace(variant_key, encoded);
        if (!inserted && where->second != encoded) {
            collision = true;
            return;
        }
        BattleTurnConcreteVariant variant{
            .commands = std::move(commands),
            .encoded_commands = encoded,
            .variant_key = variant_key,
        };
        if (!context || ClassifyBattleTurnTargets(context, variant.commands)
                == BattleTargetAvailability::Available) {
            compilation.context_viable_variants.push_back(variant);
        }
        compilation.structural_variants.push_back(std::move(variant));
    };

    const auto recurse = [&](const auto& self, std::size_t index) -> void {
        if (collision) return;
        if (index == planned.size()) {
            emit();
            return;
        }
        const auto& item = planned[index];
        current[index] = item.base;
        if (!item.needs_target || item.same_as_actor) {
            self(self, index + 1);
            return;
        }
        for (const int target : item.direct_domain) {
            current[index] = item.base;
            current[index].params.target_slot = static_cast<std::uint8_t>(target);
            direct_target_by_actor[item.source.actor_slot] = target;
            self(self, index + 1);
            direct_target_by_actor.erase(item.source.actor_slot);
        }
    };
    recurse(recurse, 0);
    if (collision) return fail("Battle target variant hash collision detected");
    if (compilation.structural_variants.empty())
        return fail("Battle Plan produced no concrete target variants");
    if (!context)
        compilation.context_viable_variants = compilation.structural_variants;
    if (error_out) error_out->clear();
    return compilation;
}

namespace {

using namespace savor::runtime;
using namespace savor::runtime::program;
using namespace savor::runtime::predicates;

constexpr std::string_view kStepKind = "battle.single_turn";
constexpr std::string_view kStartStepKind = "battle.start";
constexpr std::string_view kWaveRefKind = "analysis_battle.turn_wave";
constexpr std::string_view kBattleSetRefKind = "analysis_battle.battle_set";
constexpr std::string_view kTurnJobRefKind = "analysis_battle.turn_job";
constexpr std::string_view kPurpose = "BATTLE_SINGLE_TURN";
constexpr std::string_view kCreatedBy = "battle_single_turn_program_kind";
constexpr std::string_view kBattleSetOutputKey = "battle_set";
constexpr std::string_view kBattleSetDataKind = "analysis_battle.battle_set";
constexpr std::size_t kDeclaredTerminalBytes = 4ull * 1024ull * 1024ull;

const WorksetObservationDefaultsV1& ObservationDefaults()
{
    static const WorksetObservationDefaultsV1 defaults{
        .progress_library_ids = {
            "soa.progress.battle.events/1",
            "soa.progress.predicate.evaluations/1",
        },
    };
    return defaults;
}

std::int64_t NowMs() { return types::UtcNow().time_since_epoch().count(); }

bool Fail(std::string message, std::string* error_out) {
    if (error_out) *error_out = std::move(message);
    return false;
}

bool ValidateBattlePlanForMaterialization(
    const BattlePlanSnapshot& plan,
    std::string* error_out) {
    if (!ValidateBattlePlanTurnSequence(plan, error_out)) return false;
    for (const auto& turn : plan.turns) {
        if (!CompileBattleTurnVariants(turn, nullptr, error_out)) return false;
    }
    return true;
}

std::filesystem::path WorkingRoot(const std::filesystem::path& configured) {
    return configured.empty()
        ? std::filesystem::temp_directory_path() / "savor-battle-single-turn"
        : configured;
}

bool IsLowerHexSha256(std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

std::optional<std::string> HashFile(const std::filesystem::path& path) {
    try { return hash::sha256_of_file(path.string()); }
    catch (...) { return std::nullopt; }
}

bool ExactSavestate(const SavestateRecord& state, const std::filesystem::path& path) {
    std::error_code error;
    return state.is_complete && state.artifact_size_bytes > 0
        && IsLowerHexSha256(state.artifact_sha256)
        && std::filesystem::is_regular_file(path, error) && !error
        && static_cast<std::int64_t>(std::filesystem::file_size(path, error))
            == state.artifact_size_bytes && !error
        && HashFile(path) == std::optional<std::string>(state.artifact_sha256);
}

std::optional<std::filesystem::path> ResolveSavestate(
    const SavestateRecord& state,
    const std::filesystem::path& root,
    IStateDb* state_db,
    std::string* error_out) {
    const std::filesystem::path recorded(state.artifact_filename);
    if (ExactSavestate(state, recorded)) return recorded;
    const auto destination = WorkingRoot(root) / "baselines"
        / (std::to_string(state.savestate_id) + "-" + state.artifact_sha256 + ".sav");
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        Fail("failed creating battle.single_turn baseline directory: " + error.message(), error_out);
        return std::nullopt;
    }
    if (!ExactSavestate(state, destination)) {
        const auto materialized = state_db->MaterializeSavestateToPath(
            state.savestate_id, destination.string(), error_out);
        if (!materialized || !ExactSavestate(state, destination)) {
            if (error_out && error_out->empty())
                *error_out = "State DB did not materialize the exact battle.single_turn baseline";
            return std::nullopt;
        }
    }
    return destination;
}

std::optional<soa::battle::ctx::BattleContext> ReadBattleContextArtifact(
    IStateDb* state_db,
    std::optional<std::int64_t> artifact_id) {
    if (!state_db || !artifact_id) return std::nullopt;
    const auto artifact = state_db->GetArtifact(*artifact_id);
    if (!artifact || artifact->artifact_kind != "BATTLE_CONTEXT"
        || artifact->file_ext != soa::battle::ctx::codec::ext
        || artifact->size_bytes <= 0 || !IsLowerHexSha256(artifact->sha256)) {
        return std::nullopt;
    }
    const std::filesystem::path path(artifact->filename);
    std::error_code file_error;
    if (!std::filesystem::is_regular_file(path, file_error) || file_error
        || static_cast<std::int64_t>(std::filesystem::file_size(path, file_error))
            != artifact->size_bytes || file_error
        || HashFile(path) != std::optional<std::string>(artifact->sha256)) {
        return std::nullopt;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(artifact->size_bytes), '\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(bytes.size()))
        return std::nullopt;
    soa::battle::ctx::BattleContext context{};
    if (!soa::battle::ctx::codec::decode(bytes, context)) return std::nullopt;
    return context;
}

std::optional<std::int64_t> WaveId(const ProgramJobMaterializationContext& context) {
    if (context.step.domain_ref_id > 0) return context.step.domain_ref_id;
    if (!context.graph) return std::nullopt;
    for (const auto& binding : context.graph->input_bindings) {
        if (binding.ref_kind == kWaveRefKind && binding.ref_id > 0)
            return binding.ref_id;
    }
    return std::nullopt;
}

std::optional<std::int64_t> ExactGraphInput(
    const ProgramJobMaterializationContext& context,
    std::string_view input_key,
    std::string_view data_kind,
    std::string_view ref_kind) {
    if (!context.graph) return std::nullopt;
    for (const auto& binding : context.graph->input_bindings) {
        if (binding.input_key == input_key && binding.data_kind == data_kind
            && binding.ref_kind == ref_kind && binding.ref_id > 0) {
            return binding.ref_id;
        }
    }
    return std::nullopt;
}

const BattlePlanTurnSnapshot* FindTurn(const BattlePlanSnapshot& plan, int index) {
    const auto found = std::ranges::find(plan.turns, index, &BattlePlanTurnSnapshot::turn_index);
    return found == plan.turns.end() ? nullptr : &*found;
}

std::optional<std::int64_t> IntegerArgument(
    const WorkflowGraphStepScheduleContext* graph,
    std::string_view first,
    std::string_view second = {}) {
    if (!graph) return std::nullopt;
    for (const auto& argument : graph->arguments) {
        if ((argument.argument_key == first || (!second.empty() && argument.argument_key == second))
            && argument.integer_value) return argument.integer_value;
    }
    return std::nullopt;
}

std::optional<std::string> TextArgument(
    const WorkflowGraphStepScheduleContext* graph,
    std::string_view first,
    std::string_view second = {}) {
    if (!graph) return std::nullopt;
    for (const auto& argument : graph->arguments) {
        if ((argument.argument_key == first || (!second.empty() && argument.argument_key == second))
            && argument.text_value) return argument.text_value;
    }
    return std::nullopt;
}

template <typename T>
std::optional<T> CheckedInteger(std::int64_t value) {
    if constexpr (std::is_unsigned_v<T>) {
        if (value < 0 || static_cast<std::uint64_t>(value) > std::numeric_limits<T>::max())
            return std::nullopt;
    } else if (value < std::numeric_limits<T>::min() || value > std::numeric_limits<T>::max()) {
        return std::nullopt;
    }
    return static_cast<T>(value);
}

std::optional<PredicateExecutionPackageV1> PreparePredicatePackage(
    IAuthoringDb* authoring_db,
    std::optional<std::int64_t> group_revision_id,
    std::string* error_out) {
    if (!group_revision_id) return EmptyPredicateExecutionPackageV1();
    if (!authoring_db) {
        Fail("predicate group resolution requires Authoring DB", error_out);
        return std::nullopt;
    }
    const auto authored = authoring_db->GetPredicateGroupRevision(*group_revision_id);
    if (!authored || authored->revision_state != "PUBLISHED") {
        Fail("Battle predicate group revision is not published", error_out);
        return std::nullopt;
    }

    std::set<std::int64_t> required_binding_ids;
    for (const auto& member : authored->group.members) {
        required_binding_ids.insert(member.execution_binding_revision_id);
        if (member.guard_execution_binding_revision_id)
            required_binding_ids.insert(*member.guard_execution_binding_revision_id);
    }
    PredicateExecutionPackageV1 package{
        .hook_contract = BattlePredicateHookContractV1(),
        .group = authored->group,
    };
    package.execution_bindings.reserve(required_binding_ids.size());
    for (const auto binding_id : required_binding_ids) {
        const auto binding = authoring_db->GetPredicateExecutionBindingRevision(binding_id);
        if (!binding || binding->revision_state != "PUBLISHED") {
            Fail("predicate group references an unpublished execution binding", error_out);
            return std::nullopt;
        }
        package.execution_bindings.push_back(binding->binding);
    }
    package.content_sha256 = ComputePredicateExecutionPackageHashV1(package);
    const auto validation = ValidatePredicateExecutionPackageV1(package);
    if (!validation) {
        Fail(validation.code + ": " + validation.message, error_out);
        return std::nullopt;
    }
    return package;
}

std::optional<PredicateExecutionPackageV1> ReconstructPredicatePackage(
    const BattlePredicateExecutionPackageSnapshot& stored,
    std::string* error_out) {
    PredicateExecutionPackageV1 package{};
    std::string diagnostic;
    if (!DecodePredicateExecutionPackageV1(stored.execution_package_blob, package, &diagnostic)) {
        Fail("persisted predicate execution package is malformed: " + diagnostic, error_out);
        return std::nullopt;
    }
    const auto expected_group_id = stored.predicate_group_revision_id.value_or(
        EmptyPredicateGroupV1().predicate_group_revision_id);
    if (package.group.predicate_group_revision_id != expected_group_id
        || package.group.content_sha256 != stored.predicate_group_sha256
        || package.content_sha256 != stored.execution_package_sha256
        || package.hook_contract.canonical_id != stored.hook_contract_canonical_id
        || package.hook_contract.revision != stored.hook_contract_revision
        || package.hook_contract.content_sha256 != stored.hook_contract_sha256) {
        Fail("persisted predicate execution package lineage drifted", error_out);
        return std::nullopt;
    }
    return package;
}

std::string JobInput(std::int64_t turn_job_id,
                     std::string_view binding_hash,
                     std::string_view phase_hash) {
    return "BST1:" + std::to_string(turn_job_id) + ":"
        + std::string(binding_hash) + ":" + std::string(phase_hash);
}

std::string Fingerprint(std::int64_t wave_id,
                        std::string_view commands,
                        int fake_attacks,
                        std::string_view binding_hash,
                        std::string_view phase_hash) {
    const std::string canonical = std::to_string(wave_id) + "\n"
        + std::string(commands) + "\n" + std::to_string(fake_attacks) + "\n"
        + std::string(binding_hash) + "\n" + std::string(phase_hash);
    return "PK=1;PV=1;bst=" + hash::sha256(canonical.data(), canonical.size());
}

struct WaveExecutionSource {
    BattleTurnWaveSnapshot wave;
    BattleSetSnapshot battle_set;
    BattleSeedCandidateRow seed_candidate;
    SavestateRecord savestate;
    std::optional<std::int64_t> parent_exec_job_id;
    std::uint32_t cumulative_fake_attacks_before = 0;
};

std::optional<soa::battle::ctx::BattleContext> ResolveWavePlanningContext(
    const WaveExecutionSource& source,
    IAnalysisDb* analysis_db,
    IStateDb* state_db) {
    std::optional<std::int64_t> artifact_id;
    if (source.wave.turn_index == 1) {
        const auto probe = source.wave.context_probe_id && analysis_db
            ? analysis_db->GetBattleContextProbe(*source.wave.context_probe_id)
            : std::nullopt;
        if (probe && probe->probe_status == BattleContextProbeStatus::Succeeded)
            artifact_id = probe->context_artifact_id;
    } else if (source.wave.parent_turn_job_id && analysis_db) {
        const auto parent = analysis_db->GetBattleTurnJob(
            *source.wave.parent_turn_job_id);
        const auto result = parent && parent->exec_job_id
            ? analysis_db->GetBattleSingleTurnResultForExecJob(
                *parent->exec_job_id)
            : std::nullopt;
        if (result && result->terminal_kind == "SUCCEEDED"
            && result->domain_outcome
                == std::optional<std::string>("ReachedNextTurn")) {
            artifact_id = result->battle_context_artifact_id;
        }
    }
    return ReadBattleContextArtifact(state_db, artifact_id);
}

std::optional<WaveExecutionSource> ResolveWaveSource(
    std::int64_t wave_id,
    IAnalysisDb* analysis_db,
    IStateDb* state_db,
    std::string* error_out) {
    const auto wave = analysis_db ? analysis_db->GetBattleTurnWave(wave_id) : std::nullopt;
    const auto set = wave ? analysis_db->GetBattleSet(wave->battle_set_id) : std::nullopt;
    const auto candidate = wave ? analysis_db->GetBattleSeedCandidate(wave->seed_candidate_id) : std::nullopt;
    if (!wave || !set || !candidate || wave->turn_index <= 0
        || candidate->battle_set_id != set->battle_set_id) {
        Fail("battle.single_turn wave lineage is incomplete", error_out);
        return std::nullopt;
    }
    std::int64_t source_id = 0;
    std::optional<std::int64_t> parent_exec;
    std::uint32_t cumulative = 0;
    if (wave->turn_index == 1) {
        if (wave->parent_turn_job_id || wave->parent_wave_id
            || candidate->source_kind != BattleSeedCandidateSourceKind::SeedProbeConfirmedResult
            || !candidate->source_probe_result_id || !candidate->source_input_frame_id) {
            Fail("first-turn wave must be seeded only by a confirmed SeedProbe result", error_out);
            return std::nullopt;
        }
        const auto probe = analysis_db->GetSeedProbeResult(*candidate->source_probe_result_id);
        if (!probe || probe->evidence_state != SeedProbeEvidenceState::Confirmed
            || probe->input_frame_id != *candidate->source_input_frame_id
            || static_cast<std::int64_t>(probe->seed_value) != candidate->seed_value) {
            Fail("first-turn SeedProbe confirmation identity drifted", error_out);
            return std::nullopt;
        }
        source_id = set->entry_savestate_id;
    } else {
        if (!wave->parent_turn_job_id || !wave->parent_wave_id) {
            Fail("later-turn wave requires an exact parent candidate", error_out);
            return std::nullopt;
        }
        const auto parent = analysis_db->GetBattleTurnJob(*wave->parent_turn_job_id);
        if (!parent || parent->wave_id != *wave->parent_wave_id
            || parent->job_state != BattleTurnJobState::Succeeded
            || parent->battle_outcome != BattleTurnOutcome::ReachedNextTurn
            || !parent->output_savestate_id || !parent->exec_job_id) {
            Fail("later-turn parent did not reach TurnInputs with a successor", error_out);
            return std::nullopt;
        }
        source_id = *parent->output_savestate_id;
        parent_exec = parent->exec_job_id;
        const auto total = static_cast<std::int64_t>(parent->fake_attacks_used_before)
            + static_cast<std::int64_t>(parent->fake_attacks_this_turn);
        if (total < 0 || total > std::numeric_limits<std::uint32_t>::max()) {
            Fail("cumulative fake-attack lineage overflowed", error_out);
            return std::nullopt;
        }
        cumulative = static_cast<std::uint32_t>(total);
    }
    const auto state = state_db ? state_db->GetSavestate(source_id) : std::nullopt;
    if (!state || !state->is_complete || state->artifact_kind != "SAV"
        || state->playback_state != SavestatePlaybackState::MovieInactive
        || state->dtm_artifact_id) {
        Fail("battle.single_turn requires one complete movie-inactive source savestate", error_out);
        return std::nullopt;
    }
    return WaveExecutionSource{*wave, *set, *candidate, *state, parent_exec, cumulative};
}

GCInputFrame SeedFrame(const AnalysisInputSetFrameRow& row) {
    const auto byte = [](std::int32_t value) {
        return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
    };
    GCInputFrame frame{};
    frame.buttons = 0;
    frame.main_x = byte(row.main_x);
    frame.main_y = byte(row.main_y);
    frame.c_x = byte(row.cstick_x);
    frame.c_y = byte(row.cstick_y);
    frame.trig_l = byte(row.trigger_x);
    frame.trig_r = byte(row.trigger_y);
    return frame;
}

ProgramResultDecision FinalDecision(
    std::string state,
    std::optional<std::string> code = std::nullopt,
    std::optional<std::string> text = std::nullopt) {
    ProgramResultDecision result{};
    result.final_job_state = std::move(state);
    result.error_code = std::move(code);
    result.error_text = std::move(text);
    return result;
}

bool WriteBytesAtomically(const std::filesystem::path& destination,
                          std::span<const std::uint8_t> bytes,
                          std::string* error_out) {
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) return Fail("failed creating artifact directory: " + error.message(), error_out);
    const std::string expected = hash::sha256(bytes.data(), bytes.size());
    if (std::filesystem::is_regular_file(destination, error) && !error
        && HashFile(destination) == std::optional<std::string>(expected)) return true;
    const auto temporary = std::filesystem::path(destination.string() + ".publishing");
    std::filesystem::remove(temporary, error);
    error.clear();
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return Fail("failed opening temporary artifact", error_out);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) return Fail("failed writing temporary artifact", error_out);
    }
    std::filesystem::rename(temporary, destination, error);
    if (!error) return true;
    if (std::filesystem::is_regular_file(destination)
        && HashFile(destination) == std::optional<std::string>(expected)) {
        std::filesystem::remove(temporary, error);
        return true;
    }
    return Fail("failed publishing artifact: " + error.message(), error_out);
}

bool WriteTextAtomically(const std::filesystem::path& destination,
                         std::string_view bytes,
                         std::string* error_out) {
    return WriteBytesAtomically(destination,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()), error_out);
}

std::string OutcomeName(runtime::battlesingleturn::BattleSingleTurnOutcomeV1 outcome) {
    using Outcome = runtime::battlesingleturn::BattleSingleTurnOutcomeV1;
    switch (outcome) {
    case Outcome::ReachedNextTurn: return "ReachedNextTurn";
    case Outcome::Victory: return "Victory";
    case Outcome::Defeat: return "Defeat";
    case Outcome::PredicateRejected: return "PredicateRejected";
    }
    return "Unknown";
}

BattleTurnOutcome LegacyOutcome(runtime::battlesingleturn::BattleSingleTurnOutcomeV1 outcome) {
    using Outcome = runtime::battlesingleturn::BattleSingleTurnOutcomeV1;
    switch (outcome) {
    case Outcome::ReachedNextTurn: return BattleTurnOutcome::ReachedNextTurn;
    case Outcome::Victory: return BattleTurnOutcome::Victory;
    case Outcome::Defeat: return BattleTurnOutcome::Defeat;
    case Outcome::PredicateRejected: return BattleTurnOutcome::PredFailure;
    }
    return BattleTurnOutcome::Unknown;
}

bool IsCommandEntryFailure(const ProgramResult& result,
                           std::string_view terminal_code,
                           std::string_view terminal_message) {
    const auto matches = [](std::string_view value) {
        return value.find("interaction") != std::string_view::npos
            || value.find("command_entry") != std::string_view::npos
            || value.find("turn_is_ready") != std::string_view::npos
            || value.find("input_publish") != std::string_view::npos
            || value.find("input_await") != std::string_view::npos;
    };
    if (matches(terminal_code) || matches(terminal_message)) return true;
    return std::ranges::any_of(result.diagnostics, [&](const ProgramDiagnostic& diagnostic) {
        return matches(diagnostic.code) || matches(diagnostic.message);
    });
}

class Materializer final : public IProgramJobMaterializer {
public:
    Materializer(IExecutionDb* execution_db, IStateDb* state_db,
                 IAnalysisDb* analysis_db, IAuthoringDb* authoring_db,
                 BattleSingleTurnPhaseRegistrationConfig config)
        : execution_db_(execution_db), state_db_(state_db), analysis_db_(analysis_db),
          authoring_db_(authoring_db), config_(std::move(config)) {}

    bool Materialize(const ProgramJobMaterializationContext& context,
                     WorkflowStepScheduleResult* result_out,
                     std::string* error_out) const override {
        if (context.step.step_kind == kStartStepKind)
            return MaterializeStart(context, result_out, error_out);
        if (!result_out || !execution_db_ || !state_db_ || !analysis_db_
            || !authoring_db_ || context.step.step_kind != kStepKind)
            return Fail("battle.single_turn materialization is incomplete", error_out);
        *result_out = {};
        const auto wave_id = WaveId(context);
        auto source = wave_id ? ResolveWaveSource(*wave_id, analysis_db_, state_db_, error_out)
                              : std::nullopt;
        if (!source) return wave_id.has_value() ? false : Fail("battle.single_turn wave input is missing", error_out);
        const auto plan = authoring_db_->GetBattlePlan(source->battle_set.battle_plan_id);
        const auto* turn = plan ? FindTurn(*plan, source->wave.turn_index) : nullptr;
        if (!plan || plan->fingerprint != source->battle_set.battle_plan_fingerprint || !turn)
            return Fail("battle.single_turn authored Battle Plan turn is missing", error_out);
        if (!ValidateBattlePlanForMaterialization(*plan, error_out)) return false;
        const auto planning_context = ResolveWavePlanningContext(
            *source, analysis_db_, state_db_);
        auto variants = CompileBattleTurnVariants(
            *turn,
            planning_context ? &*planning_context : nullptr,
            error_out);
        if (!variants) return false;
        const auto& selected_variants = planning_context
            && !variants->context_viable_variants.empty()
            ? variants->context_viable_variants
            : variants->structural_variants;
        auto predicate_package = PreparePredicatePackage(
            authoring_db_, turn->default_predicate_group_revision_id, error_out);
        if (!predicate_package) return false;
        const bool first_turn = source->wave.turn_index == 1;
        auto phase = runtime::battlesingleturn::PrepareBattleSingleTurnFullPhaseV1(
            first_turn, *predicate_package, error_out);
        if (!phase) return false;
        const auto& identity = phase->identity();
        const auto& runtime_contract = phase->runtime_contract();
        ResolvedWorksetObservationBindingV1 observation;
        if (!ResolveWorksetObservationBindingV1(
                context,
                ObservationDefaults(),
                WorkingRoot(config_.working_dir_root) / "captures",
                &observation,
                error_out))
        {
            return false;
        }
        const auto program_package = fullphase::
            BuildFullPhaseProgramPackage(*phase);
        const std::array derived_defaults{
            std::string(runtime::derived::kBattleCoreBlockId)};
        ResolvedWorksetDerivedStateBindingV1 derived_state;
        if (!ResolveWorksetDerivedStateBindingV1(
                derived_defaults,
                program_package,
                &derived_state,
                error_out))
        {
            return false;
        }

        std::vector<std::uint8_t> encoded_predicate_package;
        std::string predicate_diagnostic;
        if (!EncodePredicateExecutionPackageV1(
                *predicate_package, encoded_predicate_package,
                &predicate_diagnostic)) {
            return Fail("predicate execution package encoding failed: "
                + predicate_diagnostic, error_out);
        }
        BindBattlePredicateExecutionPackageCommand binding{};
        binding.wave_id = source->wave.wave_id;
        binding.predicate_group_revision_id = turn->default_predicate_group_revision_id;
        binding.predicate_group_sha256 = predicate_package->group.content_sha256;
        binding.execution_package_sha256 = predicate_package->content_sha256;
        binding.execution_package_blob = std::move(encoded_predicate_package);
        binding.phase_program_kind = identity.program_kind;
        binding.phase_program_version = identity.program_version;
        binding.phase_canonical_id = identity.canonical_id;
        binding.phase_revision = identity.contract_revision;
        binding.phase_sha256 = identity.canonical_sha256;
        binding.hook_contract_canonical_id = predicate_package->hook_contract.canonical_id;
        binding.hook_contract_revision = predicate_package->hook_contract.revision;
        binding.hook_contract_sha256 = predicate_package->hook_contract.content_sha256;
        binding.created_at_utc = types::UtcNow();
        if (!analysis_db_->BindBattlePredicateExecutionPackage(
                binding, nullptr, error_out)) return false;

        const int minimum = std::min(source->battle_set.launch_fake_attack_min,
                                     source->battle_set.launch_fake_attack_max);
        const int maximum = std::max(source->battle_set.launch_fake_attack_min,
                                     source->battle_set.launch_fake_attack_max);
        if (source->cumulative_fake_attacks_before > static_cast<std::uint32_t>(maximum))
            return Fail("battle.single_turn cumulative fake attacks exceed the authored maximum", error_out);
        const int remaining = maximum - static_cast<int>(source->cumulative_fake_attacks_before);
        const int first_fake = std::max(0, minimum - static_cast<int>(source->cumulative_fake_attacks_before));
        const auto fake_count = static_cast<std::uint64_t>(remaining - first_fake + 1);
        if (fake_count == 0)
            return Fail("battle.single_turn fake-attack range is empty", error_out);
        if (selected_variants.size()
            > static_cast<std::size_t>(
                std::numeric_limits<int>::max() / fake_count)) {
            return Fail("battle.single_turn target and fake-attack population overflowed", error_out);
        }
        const int expected = static_cast<int>(
            selected_variants.size() * fake_count);
        const runtime::WorkerWorksetLimits hard_limits{};
        if (static_cast<std::uint32_t>(expected)
                > hard_limits.maximum_items_per_workset
            || static_cast<std::uint64_t>(expected) * kDeclaredTerminalBytes
                > runtime::kMaximumWorksetTerminalReservationBytes) {
            return Fail(
                "battle.single_turn target and fake-attack population exceeds one workset's fixed limits",
                error_out);
        }

        const std::string materialization_key = "battle.single_turn.wave."
            + std::to_string(source->wave.wave_id);
        EnsureMaterializingJobSetReceipt ensured{};
        if (!execution_db_->EnsureMaterializingJobSet({
                .materialization_key = materialization_key,
                .program_kind = static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner),
                .purpose = std::string(kPurpose),
                .created_by = std::string(kCreatedBy),
                .created_at_utc = NowMs(),
                .priority_boost = context.step.step_priority,
                .expected_total = expected,
                .domain_ref_kind = std::string(kWaveRefKind),
                .domain_ref_id = source->wave.wave_id,
                .meta_note = "turn=" + std::to_string(source->wave.turn_index),
            }, &ensured, error_out)) return false;

        auto durable_turn_jobs = analysis_db_->ListBattleTurnJobsForWave(source->wave.wave_id);
        if (ensured.materialization_state == "MATERIALIZING") {
            for (const auto& variant : selected_variants) {
              for (int fake = first_fake; fake <= remaining; ++fake) {
                auto existing = std::ranges::find_if(durable_turn_jobs, [&](const BattleTurnJobSnapshot& job) {
                    return job.resolved_turn_commands_blob == variant.encoded_commands
                        && job.fake_attacks_this_turn == fake
                        && job.source_savestate_id == source->savestate.savestate_id;
                });
                std::int64_t turn_job_id = 0;
                std::optional<std::int64_t> exec_job_id;
                if (existing != durable_turn_jobs.end()) {
                    turn_job_id = existing->turn_job_id;
                    exec_job_id = existing->exec_job_id;
                } else {
                    if (!analysis_db_->RecordBattleTurnJob({
                            .wave_id = source->wave.wave_id,
                            .plan_id = plan->plan_id,
                            .source_savestate_id = source->savestate.savestate_id,
                            .seed_candidate_id = source->wave.seed_candidate_id,
                            .authored_plan_id = plan->plan_id,
                            .authored_turn_index = source->wave.turn_index,
                            .resolved_turn_commands_blob = variant.encoded_commands,
                            .resolved_turn_variant_key = variant.variant_key,
                            .fake_attacks_this_turn = fake,
                            .fake_attacks_used_before = static_cast<int>(source->cumulative_fake_attacks_before),
                            .job_state = BattleTurnJobState::Queued,
                            .started_at_utc = types::UtcNow(),
                            .recorded_at_utc = types::UtcNow(),
                            .correlation_id = "battle-set-" + std::to_string(source->battle_set.battle_set_id),
                            .causation_id = "wave-" + std::to_string(source->wave.wave_id),
                        }, &turn_job_id, error_out)) return false;
                }
                if (!exec_job_id) {
                    CreatePendingJobReceipt created{};
                    if (!execution_db_->CreatePendingJob({
                            .job_set_id = ensured.job_set_id,
                            .parent_job_id = source->parent_exec_job_id,
                            .program_kind = static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner),
                            .program_version = runtime::battlesingleturn::ProgramVersion,
                            .program_ref_kind = std::string(kTurnJobRefKind),
                            .program_ref_id = turn_job_id,
                            .savestate_id = source->savestate.savestate_id,
                            .fingerprint = Fingerprint(source->wave.wave_id,
                                variant.encoded_commands, fake,
                                predicate_package->content_sha256, identity.canonical_sha256),
                            .priority = context.step.step_priority,
                            .max_attempts = 1,
                            .input_ini = JobInput(turn_job_id,
                                predicate_package->content_sha256, identity.canonical_sha256),
                        }, &created, error_out)) return false;
                    if (!analysis_db_->SetBattleTurnJobExecJobId(turn_job_id, created.job_id, error_out))
                        return false;
                }
              }
            }
        }
        const auto jobs = execution_db_->ListJobsInJobSet(ensured.job_set_id);
        if (jobs.size() != static_cast<std::size_t>(expected))
            return Fail("battle.single_turn materialized job population drifted", error_out);
        std::vector<std::int64_t> ordered_jobs;
        ordered_jobs.reserve(jobs.size());
        for (const auto& job : jobs) ordered_jobs.push_back(job.job_id);
        std::ranges::sort(ordered_jobs);
        SealJobPopulationReceipt sealed{};
        if (!execution_db_->SealJobPopulation({
                .job_set_id = ensured.job_set_id,
                .expected_job_count = expected,
                .requested_by = std::string(kCreatedBy),
            }, &sealed, error_out)) return false;
        PublishWorksetWaveReceipt published{};
        if (!execution_db_->PublishWorksetWave({
                .job_set_id = ensured.job_set_id,
                .expected_job_count = expected,
                .worksets = {{
                    .job_set_id = ensured.job_set_id,
                    .workflow_step_id = context.step.workflow_step_id,
                    .workset_key = materialization_key + ".workset.0",
                    .program_kind = static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner),
                    .program_version = runtime::battlesingleturn::ProgramVersion,
                    .contract = {
                        .contract_key = "battle-single-turn:v1:wave:"
                            + std::to_string(source->wave.wave_id) + ":phase:"
                            + identity.canonical_sha256 + ":binding:"
                            + predicate_package->content_sha256,
                        .module_canonical_id = runtime_contract.module.canonical_id,
                        .module_version = static_cast<std::int32_t>(runtime_contract.module.revision),
                        .module_sha256 = runtime_contract.module.canonical_hash,
                        .entrypoint = runtime_contract.entrypoint,
                        .verified_dependency_sha256 = runtime_contract.verified_dependency_sha256,
                        .runtime_profile_sha256 = runtime_contract.runtime_profile_sha256,
                        .program_package_sha256 =
                            program_package.canonical_sha256,
                        .estimated_payload_bytes = kDeclaredTerminalBytes * ordered_jobs.size(),
                    },
                    .derived_state = {
                        .binding_payload = derived_state.encoded_binding,
                        .binding_sha256 = derived_state.binding_sha256,
                    },
                    .observation = {
                        .capture_binding_payload =
                            observation.encoded_capture_binding,
                        .capture_binding_sha256 =
                            observation.capture_binding_sha256,
                        .progress_plan_payload =
                            observation.encoded_progress_plan,
                        .progress_plan_sha256 =
                            observation.progress_plan_sha256,
                    },
                    .priority = context.step.step_priority,
                    .ordered_job_ids = ordered_jobs,
                    .requested_by = std::string(kCreatedBy),
                }},
                .requested_by = std::string(kCreatedBy),
            }, &published, error_out)) return false;
        (void)analysis_db_->UpdateBattleTurnWaveStatus(
            source->wave.wave_id, BattleTurnWaveStatus::Running, std::nullopt, nullptr);
        result_out->job_set_id = ensured.job_set_id;
        result_out->persistence = {
            .program_ref_kind = std::string(kWaveRefKind),
            .program_ref_id = source->wave.wave_id,
            .fingerprint = materialization_key,
            .program_version = runtime::battlesingleturn::ProgramVersion,
        };
        result_out->event_lines.push_back("[battle-single-turn-materialized] wave="
            + std::to_string(source->wave.wave_id)
            + " target_variants=" + std::to_string(selected_variants.size())
            + " jobs=" + std::to_string(expected));
        if (!planning_context) {
            result_out->event_lines.push_back(
                "[battle-target-context-unknown] wave="
                + std::to_string(source->wave.wave_id)
                + " materialization=structural_superset");
        } else if (variants->context_viable_variants.empty()) {
            result_out->event_lines.push_back(
                "[battle-target-context-unavailable] wave="
                + std::to_string(source->wave.wave_id)
                + " materialization=structural_fallback");
        } else if (variants->context_viable_variants.size()
                   != variants->structural_variants.size()) {
            result_out->event_lines.push_back(
                "[battle-target-context-narrowed] wave="
                + std::to_string(source->wave.wave_id)
                + " structural="
                + std::to_string(variants->structural_variants.size())
                + " viable="
                + std::to_string(variants->context_viable_variants.size()));
        }
        return true;
    }

    bool Continue(const ProgramJobContinuationContext& context,
                  ProgramJobContinuationResult* result_out,
                  std::string* error_out) const override;

private:
    bool MaterializeStart(
        const ProgramJobMaterializationContext& context,
        WorkflowStepScheduleResult* result_out,
        std::string* error_out) const {
        if (!result_out || !execution_db_ || !analysis_db_ || !authoring_db_
            || !context.graph || !context.graph->authored_ref_id
            || context.graph->authored_ref_kind
                != std::optional<std::string>("authoring.battle_plan")) {
            return Fail("battle.start coordination input is incomplete", error_out);
        }
        *result_out = {};
        const auto probe_run_id = ExactGraphInput(
            context, "seed_probe_run", "analysis.seed_probe_run", "sp_probe_run");
        const auto context_probe_id = ExactGraphInput(
            context, "battle_context", "analysis_battle.battle_context_id",
            "ab_battle_context");
        const auto plan = authoring_db_->GetBattlePlan(
            *context.graph->authored_ref_id);
        const auto run = probe_run_id
            ? analysis_db_->GetSeedProbeRun(*probe_run_id) : std::nullopt;
        if (!probe_run_id || !context_probe_id || !plan || !run)
            return Fail("battle.start requires completed SeedProbe and Battle Context outputs plus a Battle Plan", error_out);
        if (!ValidateBattlePlanForMaterialization(*plan, error_out)) return false;
        const auto continuation_text = TextArgument(&*context.graph, "continuation_mode");
        const auto continuation_mode = continuation_text
            ? ParseBattleContinuationMode(*continuation_text)
            : BattleContinuationMode::Unknown;
        if (continuation_mode == BattleContinuationMode::Unknown)
            return Fail("battle.start continuation mode is missing or invalid", error_out);
        const auto minimum_value = IntegerArgument(
            &*context.graph, "fake_attack_min").value_or(0);
        const auto maximum_value = IntegerArgument(
            &*context.graph, "fake_attack_max").value_or(minimum_value);
        const auto minimum = CheckedInteger<int>(minimum_value);
        const auto maximum = CheckedInteger<int>(maximum_value);
        if (!minimum || !maximum || *minimum < 0 || *maximum < *minimum)
            return Fail("battle.start fake-attack range is invalid", error_out);

        EnsureBattleStartReceipt joined{};
        const auto materialization_key = "battle.start.step."
            + std::to_string(context.step.workflow_step_id);
        if (!analysis_db_->EnsureBattleStart({
                .workflow_instance_id = context.step.workflow_instance_id,
                .workflow_step_id = context.step.workflow_step_id,
                .probe_run_id = *probe_run_id,
                .context_probe_id = *context_probe_id,
                .battle_set_name = materialization_key,
                .entry_savestate_id = run->entry_savestate_id,
                .battle_plan_id = plan->plan_id,
                .battle_plan_fingerprint = plan->fingerprint,
                .continuation_mode = continuation_mode,
                .launch_fake_attack_min = *minimum,
                .launch_fake_attack_max = *maximum,
                .created_at_utc = types::UtcNow(),
                .correlation_id = materialization_key,
                .causation_id = "workflow-battle-start",
            }, &joined, error_out)) return false;
        if (joined.battle_set_id <= 0 || joined.first_wave_ids.empty())
            return Fail("battle.start join produced no first-turn waves", error_out);

        EnsureMaterializingJobSetReceipt ensured{};
        if (!execution_db_->EnsureMaterializingJobSet({
                .materialization_key = materialization_key,
                .program_kind = static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner),
                .purpose = "BATTLE_START_COORDINATION",
                .created_by = std::string(kCreatedBy),
                .created_at_utc = NowMs(),
                .priority_boost = context.step.step_priority,
                .expected_total = 0,
                .domain_ref_kind = std::string(kBattleSetRefKind),
                .domain_ref_id = joined.battle_set_id,
                .meta_note = "seedprobe-context-join",
            }, &ensured, error_out)) return false;
        SealJobPopulationReceipt sealed{};
        if (!execution_db_->SealJobPopulation({
                .job_set_id = ensured.job_set_id,
                .expected_job_count = 0,
                .requested_by = std::string(kCreatedBy),
            }, &sealed, error_out)) return false;
        PublishWorksetWaveReceipt completed{};
        if (!execution_db_->PublishWorksetWave({
                .job_set_id = ensured.job_set_id,
                .expected_job_count = 0,
                .worksets = {},
                .requested_by = std::string(kCreatedBy),
            }, &completed, error_out)) return false;
        result_out->job_set_id = ensured.job_set_id;
        result_out->persistence = {
            .program_ref_kind = std::string(kBattleSetRefKind),
            .program_ref_id = joined.battle_set_id,
            .fingerprint = materialization_key,
            .program_version = runtime::battlesingleturn::ProgramVersion,
        };
        result_out->event_lines.push_back("[battle-start-joined] battle_set="
            + std::to_string(joined.battle_set_id) + " waves="
            + std::to_string(joined.first_wave_ids.size()));
        return true;
    }

    IExecutionDb* execution_db_{};
    IStateDb* state_db_{};
    IAnalysisDb* analysis_db_{};
    IAuthoringDb* authoring_db_{};
    BattleSingleTurnPhaseRegistrationConfig config_;
};

class Reconstruction final : public IWorksetReconstructionAdapter {
public:
    Reconstruction(IStateDb* state_db, IAnalysisDb* analysis_db,
                   IAuthoringDb* authoring_db, std::filesystem::path root)
        : state_db_(state_db), analysis_db_(analysis_db), authoring_db_(authoring_db),
          root_(WorkingRoot(root)) {}

    std::optional<WorksetReconstructionResult> Reconstruct(
        const WorksetReconstructionContext& context,
        std::string* error_out) const override {
        const auto fail = [&](std::string message) -> std::optional<WorksetReconstructionResult> {
            Fail(std::move(message), error_out); return std::nullopt;
        };
        if (!state_db_ || !analysis_db_ || !authoring_db_ || context.items.empty()
            || context.workset_id <= 0 || context.dispatch_attempt_id <= 0
            || context.workflow_step_id <= 0 || context.job_set_id <= 0
            || context.dispatch_token.empty() || !context.state_compatibility.Complete())
            return fail("battle.single_turn reconstruction context is incomplete");
        std::optional<std::int64_t> wave_id;
        std::vector<BattleTurnJobSnapshot> turn_jobs;
        turn_jobs.reserve(context.items.size());
        for (const auto& item : context.items) {
            if (item.program_kind != static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner)
                || item.program_version != runtime::battlesingleturn::ProgramVersion
                || item.program_ref_kind != kTurnJobRefKind || item.program_ref_id <= 0
                || item.savestate_id.value_or(0) <= 0 || item.reserved_attempt_id == 0
                || item.claim_token.empty()) return fail("battle.single_turn item identity drifted");
            const auto job = analysis_db_->GetBattleTurnJob(item.program_ref_id);
            if (!job || job->exec_job_id != item.job_id
                || job->source_savestate_id != item.savestate_id
                || !job->resolved_turn_commands_blob || !job->authored_turn_index)
                return fail("battle.single_turn durable job request drifted");
            if (!wave_id) wave_id = job->wave_id;
            if (*wave_id != job->wave_id) return fail("battle.single_turn workset mixed waves");
            turn_jobs.push_back(*job);
        }
        auto source = ResolveWaveSource(*wave_id, analysis_db_, state_db_, error_out);
        if (!source) return std::nullopt;
        for (std::size_t index = 0; index < turn_jobs.size(); ++index) {
            const auto& item = context.items[index];
            const auto& job = turn_jobs[index];
            if (job.authored_turn_index != source->wave.turn_index
                || job.fake_attacks_used_before
                    != static_cast<int>(source->cumulative_fake_attacks_before))
                return fail("battle.single_turn turn lineage drifted");
        }
        const auto stored_binding = analysis_db_->GetBattlePredicateExecutionPackageForWave(*wave_id);
        if (!stored_binding) return fail("battle.single_turn predicate binding is missing");
        auto package = ReconstructPredicatePackage(*stored_binding, error_out);
        if (!package) return std::nullopt;
        const bool first_turn = source->wave.turn_index == 1;
        auto phase = runtime::battlesingleturn::PrepareBattleSingleTurnFullPhaseV1(
            first_turn, *package, error_out);
        if (!phase) return std::nullopt;
        const auto& identity = phase->identity();
        const auto& runtime_contract = phase->runtime_contract();
        if (stored_binding->phase_program_kind != identity.program_kind
            || stored_binding->phase_program_version != identity.program_version
            || stored_binding->phase_canonical_id != identity.canonical_id
            || stored_binding->phase_revision != identity.contract_revision
            || stored_binding->phase_sha256 != identity.canonical_sha256)
            return fail("battle.single_turn prepared phase identity drifted");
        const auto path = ResolveSavestate(source->savestate, root_, state_db_, error_out);
        if (!path) return std::nullopt;
        std::optional<GCInputFrame> first_frame;
        if (first_turn) {
            const auto row = source->seed_candidate.source_input_frame_id
                ? analysis_db_->GetAnalysisInputFrame(*source->seed_candidate.source_input_frame_id)
                : std::nullopt;
            if (!row) return fail("confirmed SeedProbe input frame is missing");
            first_frame = SeedFrame(*row);
        }

        WorkerWorksetDefinition workset{};
        workset.workset_id = WorkerWorksetId(
            static_cast<std::uint64_t>(context.dispatch_attempt_id));
        workset.phase_invocation = {
            .invocation_id = {
                .workflow_step_id = static_cast<std::uint64_t>(context.workflow_step_id),
                .job_set_id = static_cast<std::uint64_t>(context.job_set_id),
            },
            .program_package = fullphase::BuildFullPhaseProgramPackage(*phase),
            .common_input = fullphase::MakeFullPhaseCommonInput(
                "soa.battle.single_turn.CommonInput", 1,
                runtime::battlesingleturn::EncodeBattleSingleTurnCommonInputV1(
                    first_turn, *package)),
        };
        workset.baseline = {
            .artifact = {
                .kind = ProgramBaselineArtifactKind::Savestate,
                .state_path = *path,
                .state_sha256 = source->savestate.artifact_sha256,
                .compatibility = context.state_compatibility,
                .lineage = {
                    .edge = runtime_contract.baseline_lineage,
                    .producer = "SavorDb.PK_BattleSingleTurnRunner",
                },
            },
            .lineage = runtime_contract.baseline_lineage,
        };
        workset.derived_state = context.derived_state;
        workset.capture = context.capture;
        workset.progress_plan = context.progress_plan;
        workset.execution_key = {
            .module = runtime_contract.module,
            .entrypoint = runtime_contract.entrypoint,
            .verified_dependency_sha256 = runtime_contract.verified_dependency_sha256,
            .runtime_profile_sha256 = runtime_contract.runtime_profile_sha256,
            .baseline = ComputeProgramBaselineKey(workset.baseline),
            .movie_policy_sha256 = runtime_contract.movie_policy_sha256,
            .service_policy_sha256 = runtime_contract.service_policy_sha256,
            .program_package_sha256 = workset.phase_invocation.program_package.canonical_sha256,
            .common_input_sha256 = workset.phase_invocation.common_input.content_sha256,
            .derived_state_binding_sha256 =
                workset.derived_state.content_sha256,
            .capture_binding_sha256 = workset.capture
                ? workset.capture->content_sha256
                : EmptyWorksetCaptureBindingHashV1(),
            .progress_plan_sha256 = workset.progress_plan.content_sha256,
        };
        workset.execution_key.canonical_sha256 =
            ComputeWorkerWorksetExecutionKeyHash(workset.execution_key);
        std::vector<std::int64_t> ordered_ids;
        for (std::size_t index = 0; index < context.items.size(); ++index) {
            const auto& item = context.items[index];
            const auto& job = turn_jobs[index];
            if (item.input_ini != JobInput(job.turn_job_id,
                    package->content_sha256, identity.canonical_sha256))
                return fail("battle.single_turn item input identity drifted");
            const auto commands = soa::battle::actions::decode_battle_turn_commands_hex(
                *job.resolved_turn_commands_blob);
            if (!commands) return fail("battle.single_turn resolved Battle Plan is malformed");
            runtime::battlesingleturn::BattleSingleTurnRequestV1 request{};
            request.turn_index = static_cast<std::uint32_t>(source->wave.turn_index);
            request.cumulative_fake_attacks_before = source->cumulative_fake_attacks_before;
            request.plan.fake_attack_count = static_cast<std::uint32_t>(
                std::max(0, job.fake_attacks_this_turn));
            request.plan.commands = *commands;
            request.confirmed_seed_frame = first_frame;
            request.output_savestate_path = (root_ / "artifacts"
                / ("job-" + std::to_string(item.job_id) + "-attempt-"
                    + std::to_string(item.reserved_attempt_id) + ".sav")).string();
            workset.items.push_back({
                .item_id = WorkerWorksetItemId(static_cast<std::uint64_t>(item.job_id)),
                .ordinal = item.workset_item_ordinal,
                .execution = {
                    .execution_id = ProgramExecutionId(static_cast<std::uint64_t>(item.job_id)),
                    .attempt_id = AttemptId(item.reserved_attempt_id),
                    .input_payload = runtime::battlesingleturn::
                        EncodeBattleSingleTurnExecutionInputV1(request),
                },
                .declared_terminal_bytes = kDeclaredTerminalBytes,
                .correlation = {
                    .durable_job_id = std::to_string(item.job_id),
                    .claim_token = item.claim_token,
                    .parent_correlation = context.contract_key,
                },
            });
            ordered_ids.push_back(item.job_id);
        }
        std::vector<std::uint8_t> encoded;
        const auto status = EncodeWorkerWorksetV4(workset, encoded);
        if (!status) return fail("battle.single_turn workset encoding failed: " + status.message);
        workset.encoded_size_bytes = encoded.size();
        return WorksetReconstructionResult{
            .workset = std::move(workset),
            .ordered_job_ids = std::move(ordered_ids),
        };
    }

private:
    IStateDb* state_db_{};
    IAnalysisDb* analysis_db_{};
    IAuthoringDb* authoring_db_{};
    std::filesystem::path root_;
};

std::optional<std::int64_t> PublishSuccessorSavestate(
    IStateDb* state_db,
    const WaveExecutionSource& source,
    std::int64_t turn_job_id,
    std::int64_t exec_job_id,
    const ArtifactReferenceValue& artifact,
    std::string* error_out) {
    const std::filesystem::path path(artifact.storage_reference);
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    const auto sha = HashFile(path);
    if (!artifact.complete || size_error || size == 0 || !sha
        || *sha != artifact.content_hash.ToHex()) {
        Fail("successor savestate does not match worker artifact evidence", error_out);
        return std::nullopt;
    }
    std::int64_t artifact_id = 0;
    if (!state_db->StoreArtifact({
            .sha256 = *sha,
            .size_bytes = static_cast<std::int64_t>(size),
            .compression_kind = 0,
            .filename = path.string(),
            .file_ext = ".sav",
            .artifact_kind = "SAV",
            .created_at_utc = types::UtcNow(),
            .correlation_id = "battle-turn-job-" + std::to_string(turn_job_id),
            .causation_id = "execution-job-" + std::to_string(exec_job_id),
        }, &artifact_id, error_out)) return std::nullopt;
    std::int64_t savestate_id = 0;
    if (!state_db->CreateSavestate({
            .artifact_id = artifact_id,
            .playback_state = SavestatePlaybackState::MovieInactive,
            .dtm_artifact_id = std::nullopt,
            .savestate_type = "BATTLE_SINGLE_TURN_SUCCESSOR",
            .note = "Exact battle.single_turn successor",
            .is_complete = true,
            .created_at_utc = types::UtcNow(),
            .correlation_id = "battle-turn-job-" + std::to_string(turn_job_id),
            .causation_id = "execution-job-" + std::to_string(exec_job_id),
        }, &savestate_id, error_out)) return std::nullopt;
    if (!state_db->DeriveSavestate({
            .from_savestate_id = source.savestate.savestate_id,
            .to_savestate_id = savestate_id,
            .method_kind = "battle.single_turn.v1",
            .source_context_kind = std::string(kTurnJobRefKind),
            .source_context_id = turn_job_id,
            .created_at_utc = types::UtcNow(),
            .correlation_id = "battle-turn-job-" + std::to_string(turn_job_id),
            .causation_id = "execution-job-" + std::to_string(exec_job_id),
        }, nullptr, error_out)) return std::nullopt;
    return savestate_id;
}

std::optional<std::int64_t> PublishTurnContext(
    IStateDb* state_db,
    const std::filesystem::path& root,
    std::int64_t turn_job_id,
    std::int64_t exec_job_id,
    const soa::battle::ctx::BattleContext& context,
    std::string_view terminal_sha,
    std::string* error_out) {
    std::string bytes;
    if (!soa::battle::ctx::codec::encode(context, bytes)) {
        Fail("BattleContextCodec rejected the successor context", error_out);
        return std::nullopt;
    }
    const auto path = root / "artifacts"
        / ("turn-job-" + std::to_string(turn_job_id) + "-" + std::string(terminal_sha) + ".bctx");
    if (!WriteTextAtomically(path, bytes, error_out)) return std::nullopt;
    std::int64_t artifact_id = 0;
    if (!state_db->StoreArtifact({
            .sha256 = hash::sha256(bytes.data(), bytes.size()),
            .size_bytes = static_cast<std::int64_t>(bytes.size()),
            .compression_kind = 0,
            .filename = path.string(),
            .file_ext = soa::battle::ctx::codec::ext,
            .artifact_kind = "BATTLE_CONTEXT",
            .created_at_utc = types::UtcNow(),
            .correlation_id = "battle-turn-job-" + std::to_string(turn_job_id),
            .causation_id = "execution-job-" + std::to_string(exec_job_id),
        }, &artifact_id, error_out)) return std::nullopt;
    return artifact_id;
}

class ResultHandler final : public IProgramResultHandler {
public:
    ResultHandler(IExecutionDb* execution_db, IStateDb* state_db,
                  IAnalysisDb* analysis_db, IAuthoringDb* authoring_db,
                  std::filesystem::path root)
        : execution_db_(execution_db), state_db_(state_db), analysis_db_(analysis_db),
          authoring_db_(authoring_db), root_(WorkingRoot(root)) {}

    ProgramResultDecision Process(const ProgramResultProcessingContext& context) const override {
        if (!execution_db_ || !state_db_ || !analysis_db_ || !authoring_db_
            || context.program_kind != static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner)
            || context.program_version != runtime::battlesingleturn::ProgramVersion
            || context.program_ref_kind != kTurnJobRefKind || context.program_ref_id <= 0)
            return FinalDecision("FAILED", "BATTLE_SINGLE_TURN_IDENTITY_INVALID",
                                 "job is not an exact battle.single_turn request");
        std::optional<std::string> replace_failed_terminal_sha256;
        if (const auto existing = analysis_db_->GetBattleSingleTurnResultForExecJob(context.job_id)) {
            if (existing->turn_job_id != context.program_ref_id)
                return FinalDecision("FAILED", "BATTLE_SINGLE_TURN_RESULT_CONFLICT",
                                     "persisted result belongs to a different terminal");
            if (existing->worker_terminal_sha256 == context.terminal.sha256)
                return DecisionFromStored(context.job_set_id, *existing);
            if (existing->terminal_kind != "FAILED")
                return FinalDecision("FAILED", "BATTLE_SINGLE_TURN_RESULT_CONFLICT",
                                     "persisted result belongs to a different terminal");
            replace_failed_terminal_sha256 =
                existing->worker_terminal_sha256;
        }
        const auto turn_job = analysis_db_->GetBattleTurnJob(context.program_ref_id);
        if (!turn_job || turn_job->exec_job_id != context.job_id)
            return FinalDecision("FAILED", "BATTLE_SINGLE_TURN_REQUEST_DRIFT",
                                 "durable turn-job identity drifted");
        auto source = ResolveWaveSource(turn_job->wave_id, analysis_db_, state_db_, nullptr);
        const auto stored_binding = analysis_db_->GetBattlePredicateExecutionPackageForWave(turn_job->wave_id);
        std::string error;
        auto package = stored_binding
            ? ReconstructPredicatePackage(*stored_binding, &error) : std::nullopt;
        auto phase = source && package
            ? runtime::battlesingleturn::PrepareBattleSingleTurnFullPhaseV1(
                source->wave.turn_index == 1, *package, &error)
            : nullptr;
        if (!source || !stored_binding || !package || !phase)
            return PersistFailure(context, *turn_job, nullptr,
                "BATTLE_SINGLE_TURN_PACKAGE_DRIFT", error.empty()
                    ? "prepared battle.single_turn package is unavailable" : error, false,
                "FAILED", replace_failed_terminal_sha256);

        runtime::DurableWorkerTerminalEnvelope terminal{};
        if (!runtime::DecodeDurableWorkerTerminalEnvelope(
                context.terminal.envelope, &terminal, &error))
            return PersistFailure(context, *turn_job, nullptr,
                "BATTLE_SINGLE_TURN_TERMINAL_INVALID", error, false,
                "FAILED", replace_failed_terminal_sha256);
        using Status = savor::wrms::InvocationTerminalStatus;
        const bool terminal_identity =
            terminal.terminal.workset_id == static_cast<std::uint64_t>(context.terminal.dispatch_attempt_id)
            && terminal.terminal.item_id == static_cast<std::uint64_t>(context.job_id)
            && terminal.terminal.invocation_id == static_cast<std::uint64_t>(context.job_id)
            && terminal.terminal.attempt_id == context.terminal.reserved_attempt_id
            && terminal.process_generation > 0 && terminal.terminal.workset_epoch > 0;
        auto decoded = DecodeProgramResultV1(terminal.terminal.result);
        const ProgramResult* generic = decoded && decoded.value ? &*decoded.value : nullptr;
        if (!terminal_identity || terminal.terminal.status != Status::Succeeded
            || terminal.terminal.unstarted
            || terminal.terminal.session_disposition != savor::wrms::SessionDispositionCode::Clean) {
            const bool command_failure = generic && IsCommandEntryFailure(
                *generic, terminal.terminal.error_code, terminal.terminal.message);
            return PersistFailure(
                context, *turn_job, generic,
                terminal.terminal.error_code.empty()
                    ? "BATTLE_SINGLE_TURN_EXECUTION_FAILED" : terminal.terminal.error_code,
                terminal.terminal.message.empty()
                    ? "battle.single_turn did not complete cleanly" : terminal.terminal.message,
                command_failure,
                "FAILED",
                replace_failed_terminal_sha256);
        }

        runtime::battlesingleturn::BattleSingleTurnResultV1 result{};
        if (!phase->DecodeProgramResult(terminal.terminal.result, result, &error))
            return PersistFailure(context, *turn_job, generic,
                "BATTLE_SINGLE_TURN_RESULT_INVALID", error, false,
                "FAILED", replace_failed_terminal_sha256);
        using Outcome = runtime::battlesingleturn::BattleSingleTurnOutcomeV1;
        const bool needs_successor = result.outcome == Outcome::ReachedNextTurn
            || result.outcome == Outcome::Victory;
        const bool needs_context = result.outcome == Outcome::ReachedNextTurn;
        if (result.vi_end < result.vi_start || result.pred_passed > result.pred_total
            || result.predicate_group_revision_id != package->group.predicate_group_revision_id
            || result.predicate_group_sha256 != package->group.content_sha256
            || result.predicate_execution_package_sha256 != package->content_sha256
            || result.has_battle_context != needs_context
            || result.artifacts.size() != (needs_successor ? 1u : 0u))
            return PersistFailure(context, *turn_job, generic,
                "BATTLE_SINGLE_TURN_RESULT_INVALID",
                "battle.single_turn outcome evidence is inconsistent", false,
                "FAILED", replace_failed_terminal_sha256);

        std::optional<std::int64_t> successor;
        if (needs_successor) {
            successor = PublishSuccessorSavestate(
                state_db_, *source, turn_job->turn_job_id, context.job_id,
                result.artifacts.front().artifact, &error);
            if (!successor) throw std::runtime_error(error);
        }
        std::optional<std::int64_t> context_artifact;
        if (needs_context) {
            context_artifact = PublishTurnContext(
                state_db_, root_, turn_job->turn_job_id, context.job_id,
                result.battle_context, context.terminal.sha256, &error);
            if (!context_artifact) throw std::runtime_error(error);
        }
        const auto clamp_int = [](std::uint64_t value) {
            return value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())
                ? std::numeric_limits<int>::max() : static_cast<int>(value);
        };
        const auto now = types::UtcNow();
        if (!analysis_db_->UpdateBattleTurnJobResult({
                .exec_job_id = context.job_id,
                .job_state = BattleTurnJobState::Succeeded,
                .ended_at_utc = now,
                .has_results = true,
                .vi_start = clamp_int(result.vi_start),
                .vi_end = clamp_int(result.vi_end),
                .delta_vi = clamp_int(result.vi_end - result.vi_start),
                .rng_seed = result.ending_rng,
                .battle_outcome = LegacyOutcome(result.outcome),
                .pred_passed = static_cast<int>(result.pred_passed),
                .pred_total = static_cast<int>(result.pred_total),
                .pred_abort_run = result.outcome == Outcome::PredicateRejected ? 1 : 0,
                .output_savestate_id = successor,
                .recorded_at_utc = now,
            }, &error)) throw std::runtime_error(error);
        std::vector<std::uint8_t> evidence;
        if ((result.outcome == Outcome::PredicateRejected ||
             !result.predicate_evidence.empty()) && generic) {
            ProgramResult predicate_result = *generic;
            predicate_result.trace.clear();
            const auto encoded = EncodeProgramResultV1(predicate_result);
            if (encoded) evidence = encoded.bytes;
        }
        const RecordBattleSingleTurnResultCommand persisted_result{
                .turn_job_id = turn_job->turn_job_id,
                .exec_job_id = context.job_id,
                .worker_terminal_sha256 = context.terminal.sha256,
                .terminal_kind = "SUCCEEDED",
                .domain_outcome = OutcomeName(result.outcome),
                .ending_rng = result.ending_rng,
                .vi_start = result.vi_start,
                .vi_end = result.vi_end,
                .pred_passed = result.pred_passed,
                .pred_total = result.pred_total,
                .cumulative_fake_attacks = result.cumulative_fake_attacks,
                .successor_savestate_id = successor,
                .battle_context_artifact_id = context_artifact,
                .predicate_group_revision_id = stored_binding->predicate_group_revision_id,
                .predicate_group_sha256 = result.predicate_group_sha256,
                .predicate_execution_package_sha256 = result.predicate_execution_package_sha256,
                .predicate_evidence_blob = std::move(evidence),
                .recorded_at_utc = now,
            };
        if (!PersistResult(
                persisted_result,
                replace_failed_terminal_sha256,
                nullptr,
                &error)) throw std::runtime_error(error);
        auto decision = FinalDecision("SUCCEEDED");
        decision.cleanup_worker_staging = true;
        if (context_artifact) {
            const auto context_path = root_ / "artifacts"
                / ("turn-job-" + std::to_string(turn_job->turn_job_id) + "-"
                    + context.terminal.sha256 + ".bctx");
            std::error_code staging_error;
            const auto staging_size = std::filesystem::file_size(
                context_path, staging_error);
            const auto staging_sha = HashFile(context_path);
            if (staging_error || !staging_sha || staging_size == 0)
                throw std::runtime_error(
                    "Battle Context staging identity is unavailable");
            decision.staging_files.push_back({
                .relative_path = context_path.lexically_relative(root_).generic_string(),
                .sha256 = *staging_sha,
                .size_bytes = static_cast<std::uint64_t>(staging_size),
            });
        }
        decision.event_lines.push_back("[battle-single-turn-completed] turn_job="
            + std::to_string(turn_job->turn_job_id) + " outcome=" + OutcomeName(result.outcome)
            + " pred_passed=" + std::to_string(result.pred_passed));
        return decision;
    }

    private:
    ProgramResultDecision PersistFailure(
        const ProgramResultProcessingContext& context,
        const BattleTurnJobSnapshot& turn_job,
        const ProgramResult* generic,
        std::string code,
        std::string message,
        bool command_entry_failure,
        std::string terminal_kind = "FAILED",
        std::optional<std::string> replace_failed_terminal_sha256 =
            std::nullopt) const {
        std::string error;
        std::vector<std::uint8_t> semantic_evidence;
        if (generic) {
            ProgramResult diagnostic_result = *generic;
            diagnostic_result.trace.clear();
            const auto encoded = EncodeProgramResultV1(diagnostic_result);
            if (encoded) semantic_evidence = encoded.bytes;
        }
        const auto now = types::UtcNow();
        if (!analysis_db_->UpdateBattleTurnJobResult({
                .exec_job_id = context.job_id,
                .job_state = BattleTurnJobState::Failed,
                .ended_at_utc = now,
                .has_results = false,
                .recorded_at_utc = now,
            }, &error)) throw std::runtime_error(error);
        const auto binding = analysis_db_->GetBattlePredicateExecutionPackageForWave(turn_job.wave_id);
        const RecordBattleSingleTurnResultCommand persisted_result{
                .turn_job_id = turn_job.turn_job_id,
                .exec_job_id = context.job_id,
                .worker_terminal_sha256 = context.terminal.sha256,
                .terminal_kind = terminal_kind,
                .error_code = code,
                .error_text = message,
                .predicate_group_revision_id = binding
                    ? binding->predicate_group_revision_id : std::nullopt,
                .predicate_group_sha256 = binding
                    ? std::optional<std::string>(binding->predicate_group_sha256) : std::nullopt,
                .predicate_execution_package_sha256 = binding
                    ? std::optional<std::string>(binding->execution_package_sha256) : std::nullopt,
                .predicate_evidence_blob = std::move(semantic_evidence),
                .recorded_at_utc = now,
            };
        if (!PersistResult(
                persisted_result,
                replace_failed_terminal_sha256,
                nullptr,
                &error)) throw std::runtime_error(error);
        auto decision = FinalDecision("FAILED", code, message);
        decision.cleanup_worker_staging = true;
        if (command_entry_failure) AddWorksetCancellations(
            context.job_set_id, context.job_id, decision);
        decision.event_lines.push_back("[battle-single-turn-failed] turn_job="
            + std::to_string(turn_job.turn_job_id) + " code=" + code
            + (command_entry_failure ? " command_entry=true" : ""));
        return decision;
    }

    bool PersistResult(
        const RecordBattleSingleTurnResultCommand& result,
        const std::optional<std::string>& replace_failed_terminal_sha256,
        std::int64_t* result_id_out,
        std::string* error_out) const {
        if (replace_failed_terminal_sha256) {
            return analysis_db_->ReplaceFailedBattleSingleTurnResult(
                {
                    .expected_worker_terminal_sha256 =
                        *replace_failed_terminal_sha256,
                    .replacement_worker_terminal_sha256 =
                        result.worker_terminal_sha256,
                    .result = result,
                    .superseded_at_utc = result.recorded_at_utc,
                },
                result_id_out,
                error_out);
        }
        return analysis_db_->RecordBattleSingleTurnResult(
            result,
            result_id_out,
            error_out);
    }

    ProgramResultDecision DecisionFromStored(
        std::int64_t job_set_id,
        const BattleSingleTurnResultSnapshot& stored) const {
        const bool succeeded = stored.terminal_kind == "SUCCEEDED";
        auto decision = FinalDecision(
            succeeded ? "SUCCEEDED" : "FAILED",
            stored.error_code,
            stored.error_text);
        if (!succeeded && stored.error_code
            && IsCommandEntryFailure(ProgramResult{}, *stored.error_code,
                                     stored.error_text.value_or("")))
            AddWorksetCancellations(job_set_id, stored.exec_job_id, decision);
        return decision;
    }

    void AddWorksetCancellations(
        std::int64_t job_set_id,
        std::int64_t failed_job_id,
        ProgramResultDecision& decision) const {
        if (!execution_db_) return;
        for (const auto& job : execution_db_->ListJobsInJobSet(job_set_id)) {
            if (job.job_id == failed_job_id
                || job.state == "SUCCEEDED" || job.state == "FAILED"
                || job.state == "INTERRUPTED" || job.state == "SUPERSEDED"
                || job.state == "CANCELED")
                continue;
            decision.cancellations.push_back({
                .job_id = job.job_id,
                .request_key = "battle-command-entry-failure:"
                    + std::to_string(failed_job_id) + ":" + std::to_string(job.job_id),
                .reason_code = "BATTLE_COMMAND_ENTRY_WORKSET_ABORT",
                .reason_text = "a command-entry canary failed; retry the complete workset after correction",
                .terminal_disposition = "AUTOMATIC_FAILURE_CASCADE",
            });
        }
    }

    IExecutionDb* execution_db_{};
    IStateDb* state_db_{};
    IAnalysisDb* analysis_db_{};
    IAuthoringDb* authoring_db_{};
    std::filesystem::path root_;
};

struct RankedCandidate {
    BattleTurnJobSnapshot job;
    BattleSingleTurnResultSnapshot result;
};

bool BetterCandidate(const RankedCandidate& lhs, const RankedCandidate& rhs) {
    const auto lhs_fake = lhs.result.cumulative_fake_attacks.value_or(
        static_cast<std::uint32_t>(std::max(0,
            lhs.job.fake_attacks_used_before + lhs.job.fake_attacks_this_turn)));
    const auto rhs_fake = rhs.result.cumulative_fake_attacks.value_or(
        static_cast<std::uint32_t>(std::max(0,
            rhs.job.fake_attacks_used_before + rhs.job.fake_attacks_this_turn)));
    const auto lhs_delta = lhs.result.vi_start && lhs.result.vi_end
        ? *lhs.result.vi_end - *lhs.result.vi_start
        : std::numeric_limits<std::uint64_t>::max();
    const auto rhs_delta = rhs.result.vi_start && rhs.result.vi_end
        ? *rhs.result.vi_end - *rhs.result.vi_start
        : std::numeric_limits<std::uint64_t>::max();
    const auto lhs_pred = lhs.result.pred_passed.value_or(0);
    const auto rhs_pred = rhs.result.pred_passed.value_or(0);
    return BattleWaveCandidateRanksBefore(
        {.cumulative_fake_attacks = lhs_fake,
         .delta_vi = lhs_delta,
         .pred_passed = lhs_pred,
         .stable_job_id = lhs.job.exec_job_id.value_or(lhs.job.turn_job_id)},
        {.cumulative_fake_attacks = rhs_fake,
         .delta_vi = rhs_delta,
         .pred_passed = rhs_pred,
         .stable_job_id = rhs.job.exec_job_id.value_or(rhs.job.turn_job_id)});
}

ProgramJobContinuationOutput BattleSetOutput(std::int64_t battle_set_id) {
    return {
        .output_key = std::string(kBattleSetOutputKey),
        .data_kind = std::string(kBattleSetDataKind),
        .ref_kind = std::string(kBattleSetRefKind),
        .ref_id = battle_set_id,
    };
}

bool IsTerminalBattleSetStatus(BattleSetStatus status) {
    return status == BattleSetStatus::Victory
        || status == BattleSetStatus::Completed
        || status == BattleSetStatus::NoSurvivors;
}

BattleTargetAvailability ClassifyTargetArtifact(
    IStateDb* state_db,
    const BattleSingleTurnResultSnapshot& result,
    const BattlePlanTurnSnapshot& turn) {
    const auto context = ReadBattleContextArtifact(
        state_db, result.battle_context_artifact_id);
    if (!context) return BattleTargetAvailability::Unknown;
    const auto variants = CompileBattleTurnVariants(turn, &*context, nullptr);
    if (!variants) return BattleTargetAvailability::Unknown;
    return variants->context_viable_variants.empty()
        ? BattleTargetAvailability::Unavailable
        : BattleTargetAvailability::Available;
}

std::optional<BattleSetStatus> AggregateBattleSetStatus(
    IAnalysisDb* analysis_db,
    std::int64_t battle_set_id,
    std::string* error_out) {
    const auto battle_set = analysis_db
        ? analysis_db->GetBattleSet(battle_set_id) : std::nullopt;
    if (!battle_set) {
        Fail("BattleSet aggregation requires a durable BattleSet", error_out);
        return std::nullopt;
    }
    if (battle_set->status == BattleSetStatus::Victory
        || battle_set->status == BattleSetStatus::Failed) {
        return battle_set->status;
    }

    const auto waves = analysis_db->ListBattleTurnWaves(battle_set_id);
    std::set<std::int64_t> parent_wave_ids;
    bool victory = false;
    for (const auto& wave : waves) {
        if (wave.parent_wave_id) parent_wave_ids.insert(*wave.parent_wave_id);
        for (const auto& job : analysis_db->ListBattleTurnJobsForWave(wave.wave_id)) {
            if (!job.exec_job_id) continue;
            const auto result = analysis_db->GetBattleSingleTurnResultForExecJob(
                *job.exec_job_id);
            if (result && result->terminal_kind == "SUCCEEDED"
                && result->domain_outcome == std::optional<std::string>("Victory")
                && result->successor_savestate_id) {
                victory = true;
            }
        }
    }

    BattleSetStatus aggregate = BattleSetStatus::NoSurvivors;
    if (victory) {
        aggregate = BattleSetStatus::Victory;
    } else {
        bool active_leaf = waves.empty();
        bool plan_complete_leaf = false;
        for (const auto& wave : waves) {
            if (parent_wave_ids.contains(wave.wave_id)) continue;
            switch (wave.status) {
            case BattleTurnWaveStatus::PlanComplete:
                plan_complete_leaf = true;
                break;
            case BattleTurnWaveStatus::NoSurvivors:
                break;
            case BattleTurnWaveStatus::Ready:
            case BattleTurnWaveStatus::Running:
            case BattleTurnWaveStatus::ContextProbing:
            case BattleTurnWaveStatus::AwaitingSelection:
            case BattleTurnWaveStatus::Selected:
            case BattleTurnWaveStatus::Completed:
            case BattleTurnWaveStatus::Unknown:
                active_leaf = true;
                break;
            }
        }
        if (active_leaf) aggregate = BattleSetStatus::Active;
        else if (plan_complete_leaf) aggregate = BattleSetStatus::Completed;
    }

    if (aggregate != battle_set->status) {
        const auto completed_at = IsTerminalBattleSetStatus(aggregate)
            ? std::optional<types::UtcTimePoint>(types::UtcNow())
            : std::nullopt;
        if (!analysis_db->UpdateBattleSetStatus(
                battle_set_id, aggregate, completed_at, error_out)) {
            return std::nullopt;
        }
    }
    return aggregate;
}

bool Materializer::Continue(
    const ProgramJobContinuationContext& context,
    ProgramJobContinuationResult* result_out,
    std::string* error_out) const {
    if (!result_out || !state_db_ || !analysis_db_ || !authoring_db_)
        return Fail("battle.single_turn continuation is incomplete", error_out);
    *result_out = {};
    if (context.materialization.step.step_kind == kStartStepKind) {
        if (context.materialization.step.domain_ref_id <= 0
            || !analysis_db_->GetBattleSet(
                context.materialization.step.domain_ref_id)) {
            return Fail("battle.start durable BattleSet is missing", error_out);
        }
        result_out->disposition = ProgramJobContinuationDisposition::Complete;
        result_out->event_lines.push_back("[battle-start-ready] battle_set="
            + std::to_string(context.materialization.step.domain_ref_id));
        return true;
    }
    const auto wave_id = WaveId(context.materialization);
    const auto wave = wave_id ? analysis_db_->GetBattleTurnWave(*wave_id) : std::nullopt;
    const auto battle_set = wave ? analysis_db_->GetBattleSet(wave->battle_set_id) : std::nullopt;
    const auto plan = battle_set
        ? authoring_db_->GetBattlePlan(battle_set->battle_plan_id) : std::nullopt;
    if (!wave || !battle_set || !plan
        || plan->fingerprint != battle_set->battle_plan_fingerprint)
        return Fail("battle.single_turn continuation authoring lineage is missing", error_out);
    if (!ValidateBattlePlanForMaterialization(*plan, error_out)) return false;
    const auto jobs = analysis_db_->ListBattleTurnJobsForWave(wave->wave_id);
    if (jobs.empty()) return Fail("battle.single_turn wave lost its candidate population", error_out);
    std::vector<RankedCandidate> results;
    results.reserve(jobs.size());
    for (const auto& job : jobs) {
        if (!job.exec_job_id) return Fail("battle.single_turn candidate has no execution identity", error_out);
        const auto result = analysis_db_->GetBattleSingleTurnResultForExecJob(*job.exec_job_id);
        if (!result) return Fail("battle.single_turn candidate result is not durable", error_out);
        if (result->error_code && IsCommandEntryFailure(
                ProgramResult{}, *result->error_code, result->error_text.value_or(""))) {
            result_out->disposition = ProgramJobContinuationDisposition::Failed;
            result_out->failure_code = "BATTLE_COMMAND_ENTRY_WORKSET_FAILED";
            result_out->failure_text =
                "command-entry canary failed; correct the interaction and explicitly retry the complete workset";
            return true;
        }
        results.push_back({job, *result});
    }
    const auto now = types::UtcNow();

    if (battle_set->status == BattleSetStatus::Victory) {
        if (!analysis_db_->UpdateBattleTurnWaveStatus(
                wave->wave_id, BattleTurnWaveStatus::Completed, now, error_out)) return false;
        const auto status = AggregateBattleSetStatus(
            analysis_db_, wave->battle_set_id, error_out);
        if (!status) return false;
        result_out->disposition = ProgramJobContinuationDisposition::Complete;
        result_out->output = BattleSetOutput(wave->battle_set_id);
        result_out->event_lines.push_back("[battle-wave-victory-absorbed] wave="
            + std::to_string(wave->wave_id));
        return true;
    }

    const auto victory = std::ranges::find_if(results, [](const RankedCandidate& candidate) {
        return candidate.result.terminal_kind == "SUCCEEDED"
            && candidate.result.domain_outcome == "Victory"
            && candidate.result.successor_savestate_id.has_value();
    });
    if (victory != results.end()) {
        if (!analysis_db_->UpdateBattleTurnWaveStatus(
                wave->wave_id, BattleTurnWaveStatus::Completed, now, error_out)) return false;
        const auto status = AggregateBattleSetStatus(
            analysis_db_, wave->battle_set_id, error_out);
        if (!status || *status != BattleSetStatus::Victory)
            return Fail("BattleSet aggregation did not preserve Victory", error_out);
        result_out->disposition = ProgramJobContinuationDisposition::Complete;
        result_out->output = BattleSetOutput(wave->battle_set_id);
        result_out->event_lines.push_back("[battle-wave-victory] wave="
            + std::to_string(wave->wave_id));
        return true;
    }

    std::vector<RankedCandidate> reached_next_turn;
    for (const auto& candidate : results) {
        if (candidate.result.terminal_kind != "SUCCEEDED"
            || candidate.result.domain_outcome != "ReachedNextTurn"
            || !candidate.result.ending_rng
            || !candidate.result.successor_savestate_id) continue;
        reached_next_turn.push_back(candidate);
    }
    if (reached_next_turn.empty()) {
        if (!analysis_db_->UpdateBattleTurnWaveStatus(
                wave->wave_id, BattleTurnWaveStatus::NoSurvivors, now, error_out)) return false;
        const auto status = AggregateBattleSetStatus(
            analysis_db_, wave->battle_set_id, error_out);
        if (!status) return false;
        result_out->disposition = ProgramJobContinuationDisposition::Complete;
        if (IsTerminalBattleSetStatus(*status))
            result_out->output = BattleSetOutput(wave->battle_set_id);
        result_out->event_lines.push_back("[battle-wave-no-survivors] wave="
            + std::to_string(wave->wave_id));
        return true;
    }

    const auto* next_turn = FindTurn(*plan, wave->turn_index + 1);
    if (!next_turn) {
        if (!analysis_db_->UpdateBattleTurnWaveStatus(
                wave->wave_id, BattleTurnWaveStatus::PlanComplete, now, error_out)) return false;
        const auto status = AggregateBattleSetStatus(
            analysis_db_, wave->battle_set_id, error_out);
        if (!status) return false;
        result_out->disposition = ProgramJobContinuationDisposition::Complete;
        if (IsTerminalBattleSetStatus(*status))
            result_out->output = BattleSetOutput(wave->battle_set_id);
        result_out->event_lines.push_back("[battle-plan-complete] wave="
            + std::to_string(wave->wave_id));
        return true;
    }
    if (!CompileBattleTurnVariants(*next_turn, nullptr, error_out)) return false;

    if (battle_set->continuation_mode == BattleContinuationMode::ManualSelection) {
        if (!analysis_db_->UpdateBattleTurnWaveStatus(
                wave->wave_id, BattleTurnWaveStatus::AwaitingSelection,
                std::nullopt, error_out)) return false;
        if (!AggregateBattleSetStatus(analysis_db_, wave->battle_set_id, error_out))
            return false;
        result_out->disposition = ProgramJobContinuationDisposition::Complete;
        result_out->event_lines.push_back("[battle-wave-awaiting-user-selection] wave="
            + std::to_string(wave->wave_id));
        return true;
    }

    std::vector<RankedCandidate> eligible;
    std::size_t unavailable_count = 0;
    std::size_t unknown_count = 0;
    for (const auto& candidate : reached_next_turn) {
        switch (ClassifyTargetArtifact(
            state_db_, candidate.result, *next_turn)) {
        case BattleTargetAvailability::Available:
            eligible.push_back(candidate);
            break;
        case BattleTargetAvailability::Unavailable:
            ++unavailable_count;
            break;
        case BattleTargetAvailability::Unknown:
            ++unknown_count;
            eligible.push_back(candidate);
            break;
        }
    }
    result_out->event_lines.push_back("[battle-target-eligibility] wave="
        + std::to_string(wave->wave_id) + " available="
        + std::to_string(eligible.size() - unknown_count) + " unavailable="
        + std::to_string(unavailable_count) + " unknown="
        + std::to_string(unknown_count));
    if (unknown_count > 0) {
        result_out->event_lines.push_back("[battle-target-context-unknown] wave="
            + std::to_string(wave->wave_id) + " count="
            + std::to_string(unknown_count)
            + " automatic_continuation=eligible");
    }
    if (eligible.empty()) {
        if (!analysis_db_->UpdateBattleTurnWaveStatus(
                wave->wave_id, BattleTurnWaveStatus::AwaitingSelection,
                std::nullopt, error_out)) return false;
        if (!AggregateBattleSetStatus(analysis_db_, wave->battle_set_id, error_out))
            return false;
        result_out->disposition = ProgramJobContinuationDisposition::Complete;
        result_out->event_lines.push_back("[battle-wave-awaiting-user-selection] wave="
            + std::to_string(wave->wave_id) + " reason=targets_unavailable");
        return true;
    }

    std::map<std::int64_t, RankedCandidate> best_by_rng;
    for (const auto& candidate : eligible) {
        auto [where, inserted] = best_by_rng.emplace(
            *candidate.result.ending_rng, candidate);
        if (!inserted && BetterCandidate(candidate, where->second))
            where->second = candidate;
    }

    std::int64_t pool_id = 0;
    if (!analysis_db_->EnsureBattleAdvancementPool({
            .battle_set_id = wave->battle_set_id,
            .turn_index = wave->turn_index,
            .pool_name = "wave-" + std::to_string(wave->wave_id) + "-ending-rng",
            .criterion_kind = BattleAdvancementCriterionKind::BestFakeAttacksByRngSeed,
            .created_at_utc = now,
            .correlation_id = "battle-set-" + std::to_string(wave->battle_set_id),
            .causation_id = "wave-" + std::to_string(wave->wave_id),
        }, &pool_id, error_out)) return false;
    auto decisions = analysis_db_->ListBattleAdvancementDecisionsForPool(pool_id);
    if (decisions.empty()) {
        std::set<std::int64_t> selected;
        for (const auto& [rng, candidate] : best_by_rng) {
            (void)rng;
            selected.insert(candidate.job.turn_job_id);
        }
        for (const auto& candidate : eligible) {
            const bool chosen = selected.contains(candidate.job.turn_job_id);
            if (!analysis_db_->RecordBattleAdvancementDecision({
                    .battle_advancement_pool_id = pool_id,
                    .turn_job_id = candidate.job.turn_job_id,
                    .decision_kind = chosen ? BattleAdvancementDecisionKind::Selected
                                            : BattleAdvancementDecisionKind::NotSelected,
                    .decision_reason = chosen
                        ? std::optional<std::string>(
                            "ending_rng winner: cumulative_fake_attacks, delta_vi, pred_passed, stable_job_id")
                        : std::optional<std::string>("ending_rng duplicate"),
                    .created_at_utc = now,
                    .correlation_id = "battle-set-" + std::to_string(wave->battle_set_id),
                    .causation_id = "battle-advancement-pool-" + std::to_string(pool_id),
                }, nullptr, error_out)) return false;
        }
        decisions = analysis_db_->ListBattleAdvancementDecisionsForPool(pool_id);
    }
    const auto all_waves = analysis_db_->ListBattleTurnWaves(wave->battle_set_id);
    std::set<std::int64_t> eligible_turn_jobs;
    for (const auto& candidate : eligible)
        eligible_turn_jobs.insert(candidate.job.turn_job_id);
    std::size_t created_count = 0;
    for (const auto& decision : decisions) {
        if (decision.decision_kind != BattleAdvancementDecisionKind::Selected) continue;
        const auto candidate = analysis_db_->GetBattleTurnJob(decision.turn_job_id);
        if (!candidate || candidate->wave_id != wave->wave_id
            || !eligible_turn_jobs.contains(candidate->turn_job_id)
            || candidate->battle_outcome != BattleTurnOutcome::ReachedNextTurn
            || !candidate->output_savestate_id) continue;
        const auto existing = std::ranges::find_if(all_waves, [&](const BattleTurnWaveSnapshot& item) {
            return item.parent_wave_id == wave->wave_id
                && item.parent_turn_job_id == candidate->turn_job_id
                && item.turn_index == wave->turn_index + 1;
        });
        if (existing != all_waves.end()) continue;
        std::int64_t child_id = 0;
        if (!analysis_db_->CreateBattleTurnWave({
                .battle_set_id = wave->battle_set_id,
                .turn_index = wave->turn_index + 1,
                .parent_wave_id = wave->wave_id,
                .parent_turn_job_id = candidate->turn_job_id,
                .seed_candidate_id = wave->seed_candidate_id,
                .battle_advancement_pool_id = pool_id,
                .status = BattleTurnWaveStatus::Ready,
                .created_at_utc = now,
                .correlation_id = "battle-set-" + std::to_string(wave->battle_set_id),
                .causation_id = "turn-job-" + std::to_string(candidate->turn_job_id),
        }, &child_id, error_out)) return false;
        ++created_count;
    }
    if (!analysis_db_->UpdateBattleTurnWaveStatus(
            wave->wave_id, BattleTurnWaveStatus::Completed, now, error_out)) return false;
    const auto status = AggregateBattleSetStatus(
        analysis_db_, wave->battle_set_id, error_out);
    if (!status) return false;
    result_out->disposition = ProgramJobContinuationDisposition::Complete;
    if (IsTerminalBattleSetStatus(*status))
        result_out->output = BattleSetOutput(wave->battle_set_id);
    result_out->event_lines.push_back("[battle-next-waves-created] wave="
        + std::to_string(wave->wave_id) + " count=" + std::to_string(created_count));
    return true;
}

class Transition final : public IWorkflowTransitionHandler {
public:
    explicit Transition(IAnalysisDb* analysis_db) : analysis_db_(analysis_db) {}

    WorkflowTransitionDecision EvaluateTransition(
        const WorkflowTransitionContext& context) const override {
        WorkflowTransitionDecision decision{};
        if (analysis_db_ && context.input_ref_id
            && context.input_ref_kind
                == std::optional<std::string>(kBattleSetRefKind)) {
            const auto set = analysis_db_->GetBattleSet(*context.input_ref_id);
            if (!set) {
                decision.blocked_reason = "battle_set_missing";
                return decision;
            }
            decision.should_advance = true;
            auto waves = analysis_db_->ListBattleTurnWaves(set->battle_set_id);
            std::ranges::sort(waves, {}, &BattleTurnWaveSnapshot::wave_id);
            for (const auto& wave : waves) {
                if (wave.turn_index != 1 || wave.parent_wave_id
                    || wave.parent_turn_job_id
                    || wave.status != BattleTurnWaveStatus::Ready) continue;
                decision.spawn_steps.push_back({
                    .step_key = "BattleTurn/t1/w" + std::to_string(wave.wave_id),
                    .step_kind = std::string(kStepKind),
                    .input_ref_kind = std::string(kWaveRefKind),
                    .input_ref_id = wave.wave_id,
                    .priority = context.priority,
                    .max_attempts = 1,
                });
            }
            if (decision.spawn_steps.empty())
                decision.blocked_reason = "battle_first_wave_missing";
            return decision;
        }
        if (!analysis_db_ || !context.input_ref_id
            || (context.input_ref_kind && *context.input_ref_kind != kWaveRefKind)) {
            decision.blocked_reason = "battle_wave_ref_missing";
            return decision;
        }
        const auto wave = analysis_db_->GetBattleTurnWave(*context.input_ref_id);
        const auto set = wave ? analysis_db_->GetBattleSet(wave->battle_set_id) : std::nullopt;
        if (!wave || !set) {
            decision.blocked_reason = "battle_wave_missing";
            return decision;
        }
        decision.should_advance = true;
        if (set->status == BattleSetStatus::Victory) return decision;
        auto waves = analysis_db_->ListBattleTurnWaves(set->battle_set_id);
        std::ranges::sort(waves, {}, &BattleTurnWaveSnapshot::wave_id);
        for (const auto& child : waves) {
            if (child.parent_wave_id != wave->wave_id
                || child.turn_index != wave->turn_index + 1
                || child.status != BattleTurnWaveStatus::Ready) continue;
            decision.spawn_steps.push_back({
                .step_key = "BattleTurn/t" + std::to_string(child.turn_index)
                    + "/w" + std::to_string(child.wave_id),
                .step_kind = std::string(kStepKind),
                .input_ref_kind = std::string(kWaveRefKind),
                .input_ref_id = child.wave_id,
                .priority = context.priority,
                .max_attempts = 1,
            });
        }
        if (set->status == BattleSetStatus::Active
            && wave->status == BattleTurnWaveStatus::AwaitingSelection
            && decision.spawn_steps.empty()) {
            decision.should_advance = false;
            decision.blocked_reason = "battle_wave_awaiting_user_selection";
        }
        return decision;
    }

private:
    IAnalysisDb* analysis_db_{};
};

} // namespace

bool BattleWaveCandidateRanksBefore(
    const BattleWaveCandidateRank& lhs,
    const BattleWaveCandidateRank& rhs) noexcept {
    if (lhs.cumulative_fake_attacks != rhs.cumulative_fake_attacks)
        return lhs.cumulative_fake_attacks < rhs.cumulative_fake_attacks;
    if (lhs.delta_vi != rhs.delta_vi)
        return lhs.delta_vi < rhs.delta_vi;
    if (lhs.pred_passed != rhs.pred_passed)
        return lhs.pred_passed > rhs.pred_passed;
    return lhs.stable_job_id < rhs.stable_job_id;
}

bool RequestBattleWaveContinuation(
    IExecutionDb* execution_db,
    IAnalysisDb* analysis_db,
    IAuthoringDb* authoring_db,
    const RequestBattleWaveContinuationCommand& command,
    RequestBattleWaveContinuationReceipt* receipt_out,
    std::string* error_out) {
    RequestBattleWaveContinuationReceipt receipt{};
    if (receipt_out) *receipt_out = receipt;
    if (!execution_db || !analysis_db || !authoring_db
        || command.workflow_instance_id <= 0
        || command.parent_workflow_step_id <= 0
        || command.parent_wave_id <= 0
        || command.selected_turn_job_ids.empty()
        || command.requested_by.empty()) {
        return Fail("manual Battle wave continuation request is incomplete", error_out);
    }
    std::set<std::int64_t> selected(
        command.selected_turn_job_ids.begin(),
        command.selected_turn_job_ids.end());
    if (selected.size() != command.selected_turn_job_ids.size()
        || *selected.begin() <= 0) {
        return Fail("manual Battle wave selection must contain unique positive turn-job ids", error_out);
    }
    const auto parent = analysis_db->GetBattleTurnWave(command.parent_wave_id);
    const auto battle_set = parent
        ? analysis_db->GetBattleSet(parent->battle_set_id) : std::nullopt;
    const auto plan = battle_set
        ? authoring_db->GetBattlePlan(battle_set->battle_plan_id)
        : std::nullopt;
    if (!parent || !battle_set || !plan
        || plan->fingerprint != battle_set->battle_plan_fingerprint
        || battle_set->continuation_mode != BattleContinuationMode::ManualSelection
        || parent->status != BattleTurnWaveStatus::AwaitingSelection
        || battle_set->status != BattleSetStatus::Active) {
        return Fail("manual Battle wave continuation requires an awaiting-selection wave in an active BattleSet", error_out);
    }
    if (!ValidateBattlePlanForMaterialization(*plan, error_out)) return false;
    const auto* next_turn = FindTurn(*plan, parent->turn_index + 1);
    if (!next_turn)
        return Fail("the authored Battle Plan has no next turn", error_out);
    if (!CompileBattleTurnVariants(*next_turn, nullptr, error_out)) return false;

    const auto parent_jobs = analysis_db->ListBattleTurnJobsForWave(parent->wave_id);
    for (const auto& job : parent_jobs) {
        if (!job.exec_job_id) continue;
        const auto result = analysis_db->GetBattleSingleTurnResultForExecJob(
            *job.exec_job_id);
        if (result && result->terminal_kind == "SUCCEEDED"
            && result->domain_outcome == std::optional<std::string>("Victory")) {
            return Fail("Victory suppresses Battle wave continuation", error_out);
        }
    }

    const auto now = types::UtcNow();
    auto all_waves = analysis_db->ListBattleTurnWaves(parent->battle_set_id);
    for (const auto turn_job_id : command.selected_turn_job_ids) {
        const auto found = std::ranges::find(
            parent_jobs, turn_job_id, &BattleTurnJobSnapshot::turn_job_id);
        if (found == parent_jobs.end() || !found->exec_job_id
            || found->battle_outcome != BattleTurnOutcome::ReachedNextTurn
            || !found->output_savestate_id) {
            return Fail("manual Battle selection contains an ineligible turn job", error_out);
        }
        const auto result = analysis_db->GetBattleSingleTurnResultForExecJob(
            *found->exec_job_id);
        if (!result || result->terminal_kind != "SUCCEEDED"
            || result->domain_outcome
                != std::optional<std::string>("ReachedNextTurn")
            || !result->successor_savestate_id) {
            return Fail("manual Battle selection lacks a durable ReachedNextTurn result", error_out);
        }
        const auto existing = std::ranges::find_if(
            all_waves, [&](const BattleTurnWaveSnapshot& wave) {
                return wave.parent_wave_id == parent->wave_id
                    && wave.parent_turn_job_id == turn_job_id
                    && wave.turn_index == parent->turn_index + 1;
            });
        if (existing != all_waves.end()) {
            receipt.child_wave_ids.push_back(existing->wave_id);
            continue;
        }
        std::int64_t child_wave_id = 0;
        if (!analysis_db->CreateBattleTurnWave({
                .battle_set_id = parent->battle_set_id,
                .turn_index = parent->turn_index + 1,
                .parent_wave_id = parent->wave_id,
                .parent_turn_job_id = turn_job_id,
                .seed_candidate_id = parent->seed_candidate_id,
                .status = BattleTurnWaveStatus::Ready,
                .created_at_utc = now,
                .correlation_id = "battle-set-"
                    + std::to_string(parent->battle_set_id),
                .causation_id = "manual-turn-job-"
                    + std::to_string(turn_job_id),
            }, &child_wave_id, error_out)) return false;
        receipt.child_wave_ids.push_back(child_wave_id);
        ++receipt.newly_created_wave_count;
        all_waves.push_back(*analysis_db->GetBattleTurnWave(child_wave_id));
    }

    auto* workflow_commands = execution_db->WorkflowCommandService();
    if (!workflow_commands)
        return Fail("workflow command service is unavailable", error_out);
    workflow::WorkflowAppendDynamicStepsCommand append{
        .workflow_instance_id = command.workflow_instance_id,
        .parent_workflow_step_id = command.parent_workflow_step_id,
        .requested_by = command.requested_by,
    };
    append.steps.reserve(receipt.child_wave_ids.size());
    for (const auto wave_id : receipt.child_wave_ids) {
        const auto wave = analysis_db->GetBattleTurnWave(wave_id);
        if (!wave) return Fail("manual Battle child wave could not be reloaded", error_out);
        append.steps.push_back({
            .step_key = "BattleTurn/t" + std::to_string(wave->turn_index)
                + "/w" + std::to_string(wave->wave_id),
            .step_kind = std::string(kStepKind),
            .input_ref_kind = std::string(kWaveRefKind),
            .input_ref_id = wave->wave_id,
            .priority = command.priority,
            .max_attempts = 1,
        });
    }
    if (!workflow_commands->AppendDynamicSteps(append, error_out)) return false;
    if (!analysis_db->UpdateBattleTurnWaveStatus(
            parent->wave_id, BattleTurnWaveStatus::Completed, now, error_out)) {
        return false;
    }
    if (!AggregateBattleSetStatus(
            analysis_db, parent->battle_set_id, error_out)) return false;
    (void)workflow_commands->AppendLifecycleEvent({
        .workflow_instance_id = command.workflow_instance_id,
        .workflow_step_id = command.parent_workflow_step_id,
        .event_kind = "Execution.BattleWaveManualContinuationRequested.v1",
        .message = "parent_wave=" + std::to_string(parent->wave_id)
            + ";selected=" + std::to_string(receipt.child_wave_ids.size()),
        .requested_by = command.requested_by,
    }, nullptr);
    if (receipt_out) *receipt_out = receipt;
    if (error_out) error_out->clear();
    return true;
}

ProgramKindDescriptor BuildBattleSingleTurnProgramDescriptor(
    IExecutionDb* execution_db,
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    IAuthoringDb* authoring_db,
    BattleSingleTurnPhaseRegistrationConfig config) {
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner);
    descriptor.program_name = "Battle Single Turn";
    descriptor.result_staging_root = config.working_dir_root;
    descriptor.full_phase_identity =
        runtime::battlesingleturn::BattleSingleTurnKindHandlerV1()->identity();
    descriptor.default_progress_library_ids =
        ObservationDefaults().progress_library_ids;
    descriptor.default_derived_state_block_ids = std::vector<std::string>{
        std::string(runtime::derived::kBattleCoreBlockId),
    };
    descriptor.default_progress_runtime_trigger_pcs =
        ObservationDefaults().runtime_sample_trigger_pcs;
    descriptor.job_materializer = std::make_shared<Materializer>(
        execution_db, state_db, analysis_db, authoring_db, config);
    descriptor.workset_reconstruction = std::make_shared<Reconstruction>(
        state_db, analysis_db, authoring_db, config.working_dir_root);
    descriptor.result_handler = std::make_shared<ResultHandler>(
        execution_db, state_db, analysis_db, authoring_db, config.working_dir_root);
    descriptor.workflow_transition = std::make_shared<Transition>(analysis_db);
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

} // namespace savor::db::execution::programdb::battle
