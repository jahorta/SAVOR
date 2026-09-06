#include "WorkflowComposition.h"
#include "../ProgramDB/ProgramKindRegistry.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#include "../../Authoring/IAuthoringDb.h"

namespace savor::db::execution::workflow {
namespace {

WorkflowPortDefinition Port(
    std::string key,
    std::string data_kind,
    std::string ref_kind,
    std::string display_name,
    bool required = true) {
    return WorkflowPortDefinition{
        .key = std::move(key),
        .data_kind = std::move(data_kind),
        .ref_kind = std::move(ref_kind),
        .display_name = std::move(display_name),
        .required = required,
    };
}

WorkflowPortDefinition Port(
    const programdb::WorkflowOutputContract& contract,
    std::string display_name,
    bool required = true) {
    return Port(std::string(contract.output_key),
        std::string(contract.data_kind), std::string(contract.ref_kind),
        std::move(display_name), required);
}

WorkflowLaunchArgumentDefinition IntegerArgument(
    std::string key,
    std::string display_name,
    bool required,
    std::optional<std::string> default_value,
    std::int64_t minimum,
    std::uint64_t maximum) {
    return WorkflowLaunchArgumentDefinition{
        .key = std::move(key),
        .display_name = std::move(display_name),
        .value_type = WorkflowLaunchArgumentValueType::Integer,
        .required = required,
        .default_value = std::move(default_value),
        .minimum_integer = minimum,
        .maximum_integer = maximum,
    };
}

WorkflowLaunchArgumentDefinition ChoiceArgument(
    std::string key,
    std::string display_name,
    bool required,
    std::optional<std::string> default_value,
    std::vector<WorkflowLaunchArgumentChoiceDefinition> choices) {
    return WorkflowLaunchArgumentDefinition{
        .key = std::move(key),
        .display_name = std::move(display_name),
        .value_type = WorkflowLaunchArgumentValueType::Choice,
        .required = required,
        .default_value = std::move(default_value),
        .choices = std::move(choices),
    };
}

WorkflowLaunchArgumentDefinition BooleanArgument(
    std::string key,
    std::string display_name,
    bool default_value) {
    return WorkflowLaunchArgumentDefinition{
        .key = std::move(key),
        .display_name = std::move(display_name),
        .value_type = WorkflowLaunchArgumentValueType::Boolean,
        .required = false,
        .default_value = default_value ? std::optional<std::string>("true")
                                       : std::optional<std::string>("false"),
    };
}

std::string BindingLabel(std::string_view node_key, std::string_view port_key) {
    return std::string(node_key) + "." + std::string(port_key);
}

std::vector<WorkflowUnitStepTemplate> SingleStep(std::string step_kind) {
    return {
        WorkflowUnitStepTemplate{
            .step_key_suffix = "",
            .step_kind = std::move(step_kind),
            .priority = 1,
            .max_attempts = 1,
        },
    };
}

WorkflowUnitDefinition SeedProbeUnit() {
    return WorkflowUnitDefinition{
        .unit_kind = "seed_probe",
        .display_name = "SeedProbe",
        .description = "Probes RNG seeds from a supported phase-entry checkpoint and selects its observation endpoint from the restored paused PC.",
        .standalone_launchable = true,
        .default_activation_params_json = "{}",
        .authored_refs = {
            { .ref_kind = "seed_probe_spec", .display_name = "Seed probe spec" },
        },
        .required_inputs = {
            Port("entry_savestate", "state.movie_inactive_savestate_id", "state.savestate", "Entry savestate"),
        },
        .possible_outputs = {
            Port(programdb::workflow_outputs::SeedProbeRun, "Confirmed SeedProbe run"),
        },
        .launch_arguments = {
            IntegerArgument("samples_per_axis", "Samples per axis", false, "5", 1, 64),
        },
        .internal_step_kinds = {
            "seedprobe.survey",
            "seedprobe.search",
            "seedprobe.confirm",
        },
        .step_templates = {
            WorkflowUnitStepTemplate{
                .step_key_suffix = "Survey",
                .step_kind = "seedprobe.survey",
                .priority = 1,
                .max_attempts = 1,
            },
        },
    };
}

} // namespace

bool WorkflowUnitRegistry::RegisterUnit(WorkflowUnitDefinition definition, std::string* error_out) {
    if (definition.unit_kind.empty()) {
        if (error_out) *error_out = "unit_kind is required";
        return false;
    }
    if (definition.display_name.empty()) {
        if (error_out) *error_out = "display_name is required for unit " + definition.unit_kind;
        return false;
    }

    std::set<std::string> input_keys;
    for (const auto& input : definition.required_inputs) {
        if (input.key.empty() || input.data_kind.empty() || input.ref_kind.empty()) {
            if (error_out) *error_out = "input key, data_kind, and ref_kind are required for unit " + definition.unit_kind;
            return false;
        }
        if (!input_keys.emplace(input.key).second) {
            if (error_out) *error_out = "duplicate input key '" + input.key + "' for unit " + definition.unit_kind;
            return false;
        }
    }

    std::set<std::string> output_keys;
    for (const auto& output : definition.possible_outputs) {
        if (output.key.empty() || output.data_kind.empty() || output.ref_kind.empty()) {
            if (error_out) *error_out = "output key, data_kind, and ref_kind are required for unit " + definition.unit_kind;
            return false;
        }
        if (!output_keys.emplace(output.key).second) {
            if (error_out) *error_out = "duplicate output key '" + output.key + "' for unit " + definition.unit_kind;
            return false;
        }
    }
    for (const auto& mapping : definition.pass_through_outputs) {
        const auto input = std::find_if(
            definition.required_inputs.begin(), definition.required_inputs.end(),
            [&](const auto& value) { return value.key == mapping.input_key; });
        const auto output = std::find_if(
            definition.possible_outputs.begin(), definition.possible_outputs.end(),
            [&](const auto& value) { return value.key == mapping.output_key; });
        if (input == definition.required_inputs.end() ||
            output == definition.possible_outputs.end() ||
            input->data_kind != output->data_kind ||
            input->ref_kind != output->ref_kind) {
            if (error_out) {
                *error_out = "invalid pass-through mapping for unit " +
                    definition.unit_kind;
            }
            return false;
        }
    }
    for (const auto& step_kind : definition.internal_step_kinds) {
        for (const auto& emitted :
             programdb::workflow_outputs::ForStepKind(step_kind)) {
            const auto declared = std::find_if(
                definition.possible_outputs.begin(),
                definition.possible_outputs.end(),
                [&](const auto& output) {
                    return output.key == emitted.output_key
                        && output.data_kind == emitted.data_kind
                        && output.ref_kind == emitted.ref_kind;
                });
            if (declared == definition.possible_outputs.end()) {
                if (error_out) {
                    *error_out = "workflow output contract mismatch for "
                        + definition.unit_kind + "/" + step_kind + ": "
                        + std::string(emitted.output_key);
                }
                return false;
            }
        }
    }
    const bool has_presentation_key =
        !definition.standalone_presentation_family_key.empty();
    const bool has_presentation_name =
        !definition.standalone_presentation_family_display_name.empty();
    if (has_presentation_key != has_presentation_name) {
        if (error_out) *error_out =
            "standalone presentation family key and display name must be provided together for unit " +
            definition.unit_kind;
        return false;
    }
    if (has_presentation_key && (!definition.standalone_launchable || definition.hidden)) {
        if (error_out) *error_out =
            "standalone presentation families require a visible standalone-launchable unit: " +
            definition.unit_kind;
        return false;
    }
    if (has_presentation_key && definition.required_inputs.size() != 1u) {
        if (error_out) *error_out =
            "standalone presentation family members require exactly one typed input: " +
            definition.unit_kind;
        return false;
    }
    if (has_presentation_key) {
        for (const auto& [_, existing] : units_) {
            if (existing.standalone_presentation_family_key ==
                    definition.standalone_presentation_family_key &&
                existing.standalone_presentation_family_display_name !=
                    definition.standalone_presentation_family_display_name) {
                if (error_out) *error_out =
                    "standalone presentation family display name conflicts for " +
                    definition.standalone_presentation_family_key;
                return false;
            }
        }
    }

    std::set<std::string> argument_keys;
    for (const auto& argument : definition.launch_arguments) {
        if (argument.key.empty() || argument.display_name.empty()) {
            if (error_out) *error_out = "launch argument key and display name are required for unit " + definition.unit_kind;
            return false;
        }
        if (!argument_keys.emplace(argument.key).second) {
            if (error_out) *error_out = "duplicate launch argument key '" + argument.key + "' for unit " + definition.unit_kind;
            return false;
        }
        if (argument.value_type == WorkflowLaunchArgumentValueType::Integer) {
            if (!argument.minimum_integer.has_value() || !argument.maximum_integer.has_value()
                || *argument.minimum_integer < 0
                || static_cast<std::uint64_t>(*argument.minimum_integer) > *argument.maximum_integer) {
                if (error_out) *error_out = "invalid integer bounds for launch argument '" + argument.key + "'";
                return false;
            }
            if (argument.default_value.has_value()) {
                std::uint64_t value = 0;
                const auto* begin = argument.default_value->data();
                const auto* end = begin + argument.default_value->size();
                const auto parsed = std::from_chars(begin, end, value);
                if (parsed.ec != std::errc{} || parsed.ptr != end
                    || value < static_cast<std::uint64_t>(*argument.minimum_integer)
                    || value > *argument.maximum_integer) {
                    if (error_out) *error_out = "invalid default for launch argument '" + argument.key + "'";
                    return false;
                }
            }
        } else {
            if (argument.minimum_integer.has_value() || argument.maximum_integer.has_value()) {
                if (error_out) *error_out = "numeric bounds require an integer launch argument: " + argument.key;
                return false;
            }
            if (argument.value_type == WorkflowLaunchArgumentValueType::Choice) {
                if (argument.choices.empty()) {
                    if (error_out) *error_out = "choice launch argument has no choices: " + argument.key;
                    return false;
                }
                std::set<std::string> choice_values;
                for (const auto& choice : argument.choices) {
                    if (choice.value.empty() || choice.display_name.empty()
                        || !choice_values.emplace(choice.value).second) {
                        if (error_out) *error_out = "choice launch argument has an empty or duplicate choice: " + argument.key;
                        return false;
                    }
                }
                if (argument.default_value.has_value()
                    && choice_values.find(*argument.default_value) == choice_values.end()) {
                    if (error_out) *error_out = "choice launch argument default is not in its catalog: " + argument.key;
                    return false;
                }
            } else if (!argument.choices.empty()) {
                if (error_out) *error_out = "choices require a choice launch argument: " + argument.key;
                return false;
            }
        }
        if (argument.required && !argument.default_value.has_value()
            && argument.value_type != WorkflowLaunchArgumentValueType::Integer) {
            // A required argument without a default is valid for every type. This
            // branch intentionally documents that no synthetic default is added.
        }
    }
    for (const auto& constraint : definition.launch_argument_constraints) {
        if (constraint.lesser_or_equal_key.empty() || constraint.greater_or_equal_key.empty()
            || constraint.message.empty()
            || argument_keys.find(constraint.lesser_or_equal_key) == argument_keys.end()
            || argument_keys.find(constraint.greater_or_equal_key) == argument_keys.end()
            || constraint.lesser_or_equal_key == constraint.greater_or_equal_key) {
            if (error_out) *error_out = "invalid launch argument constraint for unit " + definition.unit_kind;
            return false;
        }
    }

    const auto inserted = units_.emplace(definition.unit_kind, std::move(definition));
    if (!inserted.second) {
        if (error_out) *error_out = "workflow unit already registered: " + inserted.first->first;
        return false;
    }
    return true;
}

const WorkflowUnitDefinition* WorkflowUnitRegistry::Find(std::string_view unit_kind) const {
    const auto it = units_.find(std::string(unit_kind));
    if (it == units_.end()) {
        return nullptr;
    }
    return &it->second;
}

std::vector<WorkflowUnitDefinition> WorkflowUnitRegistry::ListUnits() const {
    std::vector<WorkflowUnitDefinition> out;
    out.reserve(units_.size());
    for (const auto& [_, unit] : units_) {
        out.push_back(unit);
    }
    std::sort(out.begin(), out.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.display_name < rhs.display_name;
    });
    return out;
}

std::vector<WorkflowStandalonePresentationEntry>
BuildStandalonePresentationEntries(
    std::vector<WorkflowUnitDefinition> units)
{
    std::map<std::string, WorkflowStandalonePresentationEntry> by_key;
    for (auto& unit : units) {
        if (unit.hidden || !unit.standalone_launchable) continue;
        const bool family =
            !unit.standalone_presentation_family_key.empty();
        const std::string key = family
            ? unit.standalone_presentation_family_key
            : unit.unit_kind;
        auto [it, inserted] = by_key.try_emplace(
            key,
            WorkflowStandalonePresentationEntry{
                .presentation_key = key,
                .display_name = family
                    ? unit.standalone_presentation_family_display_name
                    : unit.display_name,
                .description = family ? std::string{} : unit.description,
            });
        if (!inserted && family &&
            it->second.display_name !=
                unit.standalone_presentation_family_display_name) {
            throw std::logic_error(
                "standalone presentation family has conflicting display names: " +
                key);
        }
        it->second.members.push_back(std::move(unit));
    }

    std::vector<WorkflowStandalonePresentationEntry> entries;
    entries.reserve(by_key.size());
    for (auto& [_, entry] : by_key) {
        std::sort(entry.members.begin(), entry.members.end(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.display_name < rhs.display_name;
            });
        if (entry.description.empty()) {
            entry.description =
                "Choose a typed source and launch the matching production validation workflow.";
        }
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.display_name < rhs.display_name;
    });
    return entries;
}

WorkflowUnitRegistry BuildDefaultWorkflowUnitRegistry() {
    WorkflowUnitRegistry registry;
    std::string ignored;

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "tas_movie_establish_root_cursor",
            .display_name = "TAS Movie: Establish Root Cursor",
            .description = "Establishes the handcrafted root DTM checkpoint cursor exactly once.",
            .default_activation_params_json = "{}",
            .required_inputs = {
                Port("root_dtm", "state_artifact.dtm_artifact_id", "state_artifact", "Handcrafted root DTM"),
            },
            .possible_outputs = {
                Port(programdb::workflow_outputs::TasMovieValidationAttempt, "Validation attempt"),
                Port(programdb::workflow_outputs::TasMovieRootEstablishment, "Root establishment"),
                Port(programdb::workflow_outputs::TasMovieRootDtm, "Established root DTM"),
            },
            .internal_step_kinds = { "tasmovie.establish_root_cursor" },
            .step_templates = SingleStep("tasmovie.establish_root_cursor"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "tas_movie_validate_root",
            .display_name = "TAS Movie: Validate Root",
            .description = "Validates one RTC-patched root DTM and publishes its canonical checkpoint on first success.",
            .standalone_presentation_family_key = "tas_movie_validation",
            .standalone_presentation_family_display_name = "TAS Movie Validation",
            .default_activation_params_json = "{}",
            .required_inputs = {
                Port("root_establishment", "analysis.tas_movie_root_establishment_attempt_id", "tmv_root_establishment_attempt", "Root cursor establishment"),
            },
            .possible_outputs = {
                Port(programdb::workflow_outputs::TasMovieValidationAttempt, "Validation attempt"),
                Port(programdb::workflow_outputs::TasMovieValidatedCheckpoint, "Validated checkpoint savestate"),
            },
            .launch_arguments = {
                IntegerArgument("rtc", "RTC", true, std::nullopt, 0, std::numeric_limits<std::uint32_t>::max()),
            },
            .internal_step_kinds = { "tasmovie.validate_root" },
            .step_templates = SingleStep("tasmovie.validate_root"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "tas_movie_validate_tree",
            .display_name = "Validate Recorded TAS Branch",
            .description = "Explicitly validates one immutable non-root TAS movie without capturing another checkpoint.",
            .standalone_presentation_family_key = "tas_movie_validation",
            .standalone_presentation_family_display_name = "TAS Movie Validation",
            .default_activation_params_json = "{}",
            .required_inputs = {
                Port("tas_movie_tree", "state.tas_movie_tree_id", "state_tas_movie_tree", "TAS movie tree"),
            },
            .possible_outputs = {
                Port(programdb::workflow_outputs::TasMovieValidationAttempt, "Validation attempt"),
                Port(programdb::workflow_outputs::TasMovieValidatedCheckpoint, "Validated checkpoint savestate"),
            },
            .internal_step_kinds = { "tasmovie.validate_tree" },
            .step_templates = SingleStep("tasmovie.validate_tree"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "tas_movie_checkpoint_sterilize",
            .display_name = "TAS Movie: Sterilize Checkpoint",
            .description = "Creates a canonical movie-inactive checkpoint from an exact movie-paired TAS Movie checkpoint.",
            .default_activation_params_json = "{}",
            .required_inputs = {
                Port("paired_checkpoint_savestate", "state.movie_paired_savestate_id", "state.savestate", "Movie-paired checkpoint"),
            },
            .possible_outputs = {
                Port(programdb::workflow_outputs::TasMovieSterilizedCheckpoint, "Movie-inactive checkpoint"),
            },
            .internal_step_kinds = { "tasmovie.checkpoint_sterilize" },
            .step_templates = SingleStep("tasmovie.checkpoint_sterilize"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "tas_movie_annotate",
            .display_name = "TAS Movie: Annotate Input Epochs",
            .description = "Replays a complete boot DTM and records the controller state consumed by each guest PADRead epoch.",
            .default_activation_params_json = "{}",
            .required_inputs = {
                Port("root_establishment", "analysis.tas_movie_root_establishment_attempt_id", "tmv_root_establishment_attempt", "Root establishment"),
            },
            .possible_outputs = {
                Port(programdb::workflow_outputs::TasMovieAnnotationAttempt, "Input-epoch annotation attempt"),
            },
            .internal_step_kinds = {"tasmovie.annotate"},
            .step_templates = SingleStep("tasmovie.annotate"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "tas_movie_revise",
            .display_name = "TAS Movie: Rewrite Input Epochs",
            .description = "Inserts one neutral guest-input epoch and re-emits the complete downstream annotated input schedule.",
            .default_activation_params_json = "{}",
            .required_inputs = {
                Port("annotation_attempt", "analysis.tas_movie_input_epoch_annotation_attempt_id", "tmv_input_epoch_annotation_attempt", "Source input-epoch annotation"),
            },
            .possible_outputs = {
                Port(programdb::workflow_outputs::TasMovieRewriteAttempt, "Input-epoch rewrite attempt"),
                Port(programdb::workflow_outputs::TasMovieRewrittenDtm, "Rewritten DTM"),
                Port(programdb::workflow_outputs::TasMovieRewrittenPairedSavestate, "Rewritten movie-paired endpoint"),
                Port(programdb::workflow_outputs::TasMovieAnnotationAttempt, "Rewritten input-epoch annotation"),
                Port(programdb::workflow_outputs::TasMovieRootEstablishment, "Rewritten root establishment"),
            },
            .launch_arguments = {
                IntegerArgument("insert_before_epoch", "Insert before epoch", false,
                    std::nullopt, 0,
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())),
                IntegerArgument("neutral_epoch_count", "Neutral input epochs", false,
                    "1", 1,
                    static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())),
                ChoiceArgument("placement_profile", "Placement profile", false,
                    "first_battle.final_dialog",
                    {{.value="first_battle.final_dialog",
                      .display_name="First battle: final dialog B to A"}}),
            },
            .internal_step_kinds = {"tasmovie.revise"},
            .step_templates = SingleStep("tasmovie.revise"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "tas_movie_input_epoch_breakpoint_diagnostic",
            .display_name = "TAS Movie: Input Epoch Breakpoint Diagnostic",
            .description = "Actively stops at each PADRead return to diagnose breakpoint routing without changing production annotation.",
            .hidden = true,
            .default_activation_params_json = "{}",
            .required_inputs = {
                Port("root_establishment", "analysis.tas_movie_root_establishment_attempt_id", "tmv_root_establishment_attempt", "Root establishment"),
            },
            .possible_outputs = {
                Port(programdb::workflow_outputs::TasMovieAnnotationAttempt, "Diagnostic annotation attempt"),
            },
            .internal_step_kinds = {"tasmovie.input_epoch_breakpoint_diagnostic"},
            .step_templates = SingleStep("tasmovie.input_epoch_breakpoint_diagnostic"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "tas_movie_cutscene",
            .display_name = "TAS Movie: Record Cutscene",
            .description = "Records a validated TAS branch through dialogue and stops at the next qualified pre-battle seed boundary.",
            .default_activation_params_json = "{}",
            .required_inputs = {
                Port("tas_movie_tree", "state.tas_movie_tree_id", "state_tas_movie_tree", "Validated movie-paired TAS branch"),
            },
            .possible_outputs = {
                Port(programdb::workflow_outputs::TasMovieCutsceneAttempt, "Cutscene recording attempt"),
                Port(programdb::workflow_outputs::TasMovieTree, "Recorded TAS branch"),
                Port(programdb::workflow_outputs::TasMoviePairedSavestate, "Movie-paired cutscene endpoint"),
                Port(programdb::workflow_outputs::TasMovieValidationAttempt, "Cutscene validation attempt"),
                Port(programdb::workflow_outputs::TasMovieValidatedCheckpoint, "Validated cutscene checkpoint"),
            },
            .internal_step_kinds = {"tasmovie.cutscene"},
            .step_templates = SingleStep("tasmovie.cutscene"),
        },
        &ignored);

    (void)registry.RegisterUnit(SeedProbeUnit(), &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "battle.context",
            .display_name = "Battle Context",
            .description = "Captures planning context from the exact prebattle entry state.",
            .unit_variant = "battle",
            .breakpoint_profile_key = "battle.context",
            .default_activation_params_json = "{}",
            .required_inputs = {
                Port("entry_savestate", "state.movie_inactive_savestate_id", "state.savestate", "Entry savestate"),
            },
            .possible_outputs = {
                Port(programdb::workflow_outputs::BattleContext, "Battle context"),
            },
            .internal_step_kinds = { "battle.context" },
            .step_templates = SingleStep("battle.context"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "battle",
            .display_name = "Battle",
            .description = "Runs a Battle Plan from confirmed SeedProbe and Battle Context inputs.",
            .unit_variant = "battle",
            .breakpoint_profile_key = "battle.default",
            .default_activation_params_json = "{}",
            .authored_refs = {
                { .ref_kind = "authoring.battle_plan", .display_name = "Battle plan" },
            },
            .required_inputs = {
                Port("seed_probe_run", "analysis.seed_probe_run", "sp_probe_run", "Confirmed SeedProbe run"),
                Port("battle_context", "analysis_battle.battle_context_id", "ab_battle_context", "Battle context"),
                Port("tas_root_annotation", "analysis.tas_movie_input_epoch_annotation_attempt_id",
                    "tmv_input_epoch_annotation_attempt", "TAS root annotation provenance", false),
            },
            .possible_outputs = {
                Port(programdb::workflow_outputs::BattleSet, "Battle set"),
                Port(programdb::workflow_outputs::BattleCompletion, "Battle completion"),
                Port("battle_manual_followup", "analysis.battle_manual_followup_id", "manual_followup", "Battle manual follow-up"),
            },
            .launch_arguments = {
                ChoiceArgument(
                    "continuation_mode",
                    "Continuation",
                    true,
                    std::nullopt,
                    {
                        { .value = "manual_selection", .display_name = "Manual selection after each wave" },
                        { .value = "automatic_best_per_ending_rng", .display_name = "Automatically continue the best candidate for each ending RNG" },
                    }),
                BooleanArgument(
                    "continue_automatic_exploration_after_victory",
                    "Continue automatic exploration after Victory",
                    false),
                IntegerArgument("fake_attack_min", "Minimum fake attacks", false, "0", 0, std::numeric_limits<std::int32_t>::max()),
                IntegerArgument("fake_attack_max", "Maximum fake attacks", false, "0", 0, std::numeric_limits<std::int32_t>::max()),
            },
            .launch_argument_constraints = {
                { .lesser_or_equal_key = "fake_attack_min", .greater_or_equal_key = "fake_attack_max", .message = "minimum fake attacks must not exceed maximum fake attacks" },
            },
            .internal_step_kinds = { "battle.start", "battle.single_turn", "battle.completion" },
            .step_templates = SingleStep("battle.start"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "battle_completion",
            .display_name = "Battle Completion",
            .description = "Consumes a BattleSingleTurn Victory state, applies rewards, and freezes the battle-completion manifest.",
            .hidden = true,
            .unit_variant = "battle",
            .breakpoint_profile_key = "battle.completion",
            .default_activation_params_json = "{}",
            .required_inputs = {
                Port("victory_turn_job", "analysis_battle.battle_turn_job", "analysis_battle.turn_job", "Selected Victory turn job"),
            },
            .possible_outputs = {
                Port(programdb::workflow_outputs::BattleCompletion, "Battle completion"),
            },
            .internal_step_kinds = { "battle.completion" },
            .step_templates = SingleStep("battle.completion"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "battle_recording",
            .display_name = "Battle Recording",
            .description = "Replays an explicitly completed Battle lineage and records the Battle segment through field preseed.",
            .hidden = true,
            .unit_variant = "battle",
            .breakpoint_profile_key = "battle.record",
            .default_activation_params_json = "{}",
            .required_inputs = {
                Port("completion", "analysis_battle.battle_completion", "analysis_battle.battle_completion", "Battle completion"),
            },
            .possible_outputs = {
                Port(programdb::workflow_outputs::BattleRecording, "Battle recording"),
                Port(programdb::workflow_outputs::TasMovieValidationAttempt, "Validation attempt"),
                Port(programdb::workflow_outputs::TasMovieValidatedCheckpoint, "Validated checkpoint savestate"),
            },
            .internal_step_kinds = { "battle.record" },
            .step_templates = SingleStep("battle.record"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "battle_replay",
            .display_name = "Battle Replay",
            .description = "Replays an explicitly completed Battle lineage after stopping movie playback, without producing TAS artifacts.",
            .hidden = true,
            .unit_variant = "battle",
            .breakpoint_profile_key = "battle.replay",
            .default_activation_params_json = "{}",
            .required_inputs = {
                Port("completion", "analysis_battle.battle_completion", "analysis_battle.battle_completion", "Battle completion"),
            },
            .possible_outputs = {
                Port(programdb::workflow_outputs::BattleReplay, "Battle replay"),
            },
            .internal_step_kinds = { "battle.replay" },
            .step_templates = SingleStep("battle.replay"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "navigation.context_probe",
            .display_name = "Navigation Context Probe",
            .description = "Captures the initial on-foot navigation context and a matching savestate.",
            .hidden = true,
            .unit_variant = "navigation",
            .breakpoint_profile_key = "navigation.context_probe",
            .default_activation_params_json = "{}",
            .required_inputs = {
                Port("entry_savestate", "state.movie_inactive_savestate_id", "state.savestate", "Entry savestate"),
            },
            .possible_outputs = {
                Port(
                    "navigation_context",
                    "state_artifact.navigation_context_id",
                    "state_artifact",
                    "Navigation context"),
            },
            .internal_step_kinds = { "navigation.context_probe" },
            .step_templates = SingleStep("navigation.context_probe"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "overworld_explorer",
            .display_name = "Overworld Explorer",
            .description = "Placeholder contract for future overworld traversal from a terminal savestate.",
            .hidden = true,
            .unit_variant = "overworld",
            .breakpoint_profile_key = "overworld.default",
            .default_activation_params_json = "{}",
            .required_inputs = {
                Port("entry_savestate", "state.movie_inactive_savestate_id", "state.savestate", "Entry savestate"),
            },
            .possible_outputs = {
                Port("terminal_savestate", "state.movie_inactive_savestate_id", "state.savestate", "Terminal savestate"),
            },
            .internal_step_kinds = {},
            .step_templates = SingleStep("overworld_explorer"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "dungeon_explorer",
            .display_name = "Dungeon Explorer",
            .description = "Placeholder contract for future dungeon traversal from a terminal savestate.",
            .hidden = true,
            .unit_variant = "dungeon",
            .breakpoint_profile_key = "dungeon.default",
            .default_activation_params_json = "{}",
            .required_inputs = {
                Port("entry_savestate", "state.movie_inactive_savestate_id", "state.savestate", "Entry savestate"),
            },
            .possible_outputs = {
                Port("terminal_savestate", "state.movie_inactive_savestate_id", "state.savestate", "Terminal savestate"),
            },
            .internal_step_kinds = {},
            .step_templates = SingleStep("dungeon_explorer"),
        },
        &ignored);

    return registry;
}

bool ValidateWorkflowUnitProgramOutputContracts(
    const WorkflowUnitRegistry& unit_registry,
    const programdb::ProgramKindRegistry& program_registry,
    std::string* error_out) {
    for (const auto& unit : unit_registry.ListUnits()) {
        for (const auto& step_kind : unit.internal_step_kinds) {
            const auto* descriptor = program_registry.FindForStepKind(step_kind);
            if (descriptor == nullptr) continue;
            for (const auto& emitted : descriptor->workflow_outputs) {
                const auto declared = std::find_if(
                    unit.possible_outputs.begin(), unit.possible_outputs.end(),
                    [&](const auto& output) {
                        return output.key == emitted.output_key
                            && output.data_kind == emitted.data_kind
                            && output.ref_kind == emitted.ref_kind;
                    });
                if (declared == unit.possible_outputs.end()) {
                    if (error_out) {
                        *error_out = "workflow output contract mismatch for "
                            + unit.unit_kind + "/" + step_kind + ": "
                            + std::string(emitted.output_key);
                    }
                    return false;
                }
            }
        }
    }
    if (error_out) error_out->clear();
    return true;
}

WorkflowCompositionService::WorkflowCompositionService(const WorkflowUnitRegistry* registry)
    : registry_(registry) {
}

WorkflowCompositionPreview WorkflowCompositionService::Preview(const WorkflowCompositionSpec& composition) const {
    WorkflowCompositionPreview preview{};
    if (registry_ == nullptr) {
        preview.issues.push_back({ "", "", "", "workflow unit registry unavailable" });
        return preview;
    }

    std::set<std::string> node_keys;
    for (const auto& node : composition.nodes) {
        if (node.node_key.empty()) {
            preview.issues.push_back({ "", "", "", "node_key is required" });
            continue;
        }
        if (!node_keys.emplace(node.node_key).second) {
            preview.issues.push_back({ node.node_key, "", "", "duplicate node_key" });
            continue;
        }
        const auto* unit = FindNodeUnit(node);
        if (unit == nullptr) {
            preview.issues.push_back({ node.node_key, "", "", "unknown workflow unit: " + node.unit_kind });
            continue;
        }

        WorkflowCompositionPreviewNode preview_node{};
        preview_node.node_key = node.node_key;
        preview_node.unit_kind = node.unit_kind;
        for (const auto& output : unit->possible_outputs) {
            preview_node.possible_outputs.push_back(BindingLabel(node.node_key, output.key) + " -> " + output.data_kind);
        }

        for (const auto& input : unit->required_inputs) {
            std::string resolved_label;
            WorkflowCompositionIssue issue{};
            if (ExternalBindingMatches(composition, node, input, &resolved_label)
                || OutputBindingMatches(composition, node, input, &resolved_label, &issue)) {
                preview_node.resolved_inputs.push_back(BindingLabel(node.node_key, input.key) + " <- " + resolved_label);
                continue;
            }

            if (!issue.message.empty()) {
                preview.issues.push_back(std::move(issue));
            } else if (input.required) {
                preview.issues.push_back({
                    node.node_key,
                    input.key,
                    input.data_kind,
                    "required input is not bound",
                });
            }
        }
        preview.nodes.push_back(std::move(preview_node));
    }

    preview.valid = preview.issues.empty();
    return preview;
}

std::vector<WorkflowUnitDefinition> WorkflowCompositionService::ListUnitsCompatibleWith(
    const std::vector<WorkflowPortDefinition>& available_outputs) const {
    std::vector<WorkflowUnitDefinition> out;
    if (registry_ == nullptr) {
        return out;
    }
    for (const auto& unit : registry_->ListUnits()) {
        bool all_required_satisfied = true;
        for (const auto& input : unit.required_inputs) {
            if (!input.required) {
                continue;
            }
            const auto it = std::find_if(available_outputs.begin(), available_outputs.end(), [&](const auto& output) {
                return output.data_kind == input.data_kind && output.ref_kind == input.ref_kind;
            });
            if (it == available_outputs.end()) {
                all_required_satisfied = false;
                break;
            }
        }
        if (all_required_satisfied) {
            out.push_back(unit);
        }
    }
    return out;
}

const WorkflowUnitDefinition* WorkflowCompositionService::FindNodeUnit(const WorkflowCompositionNode& node) const {
    return registry_ != nullptr ? registry_->Find(node.unit_kind) : nullptr;
}

const WorkflowPortDefinition* WorkflowCompositionService::FindInput(
    const WorkflowUnitDefinition& unit,
    std::string_view input_key) const {
    const auto it = std::find_if(unit.required_inputs.begin(), unit.required_inputs.end(), [&](const auto& input) {
        return input.key == input_key;
    });
    return it == unit.required_inputs.end() ? nullptr : &*it;
}

const WorkflowPortDefinition* WorkflowCompositionService::FindOutput(
    const WorkflowUnitDefinition& unit,
    std::string_view output_key) const {
    const auto it = std::find_if(unit.possible_outputs.begin(), unit.possible_outputs.end(), [&](const auto& output) {
        return output.key == output_key;
    });
    return it == unit.possible_outputs.end() ? nullptr : &*it;
}

bool WorkflowCompositionService::OutputBindingMatches(
    const WorkflowCompositionSpec& composition,
    const WorkflowCompositionNode& node,
    const WorkflowPortDefinition& input,
    std::string* resolved_label_out,
    WorkflowCompositionIssue* issue_out) const {
    for (const auto& binding : composition.output_bindings) {
        if (binding.to_node_key != node.node_key || binding.input_key != input.key) {
            continue;
        }

        if ((binding.guard_kind.has_value()
                && *binding.guard_kind
                    != savor::db::kWorkflowOutputPresentGuard)
            || (binding.guard_kind.has_value()
                && binding.guard_value.has_value())
            || (!binding.guard_kind.has_value()
                && binding.guard_value.has_value())) {
            if (issue_out) {
                *issue_out = {
                    node.node_key,
                    input.key,
                    input.data_kind,
                    "unsupported workflow output binding guard",
                };
            }
            return false;
        }

        const auto from_it = std::find_if(composition.nodes.begin(), composition.nodes.end(), [&](const auto& candidate) {
            return candidate.node_key == binding.from_node_key;
        });
        if (from_it == composition.nodes.end()) {
            if (issue_out) {
                *issue_out = { node.node_key, input.key, input.data_kind, "binding source node not found: " + binding.from_node_key };
            }
            return false;
        }
        const auto* from_unit = FindNodeUnit(*from_it);
        if (from_unit == nullptr) {
            if (issue_out) {
                *issue_out = { node.node_key, input.key, input.data_kind, "binding source unit not found" };
            }
            return false;
        }
        const auto* output = FindOutput(*from_unit, binding.output_key);
        if (output == nullptr) {
            if (issue_out) {
                *issue_out = { node.node_key, input.key, input.data_kind, "binding source output not found: " + binding.output_key };
            }
            return false;
        }
        if (output->data_kind != input.data_kind || output->ref_kind != input.ref_kind) {
            if (issue_out) {
                *issue_out = {
                    node.node_key,
                    input.key,
                    input.data_kind,
                    "binding type mismatch: " + output->data_kind + "/" + output->ref_kind
                        + " cannot satisfy " + input.data_kind + "/" + input.ref_kind,
                };
            }
            return false;
        }
        if (resolved_label_out) {
            *resolved_label_out = BindingLabel(binding.from_node_key, binding.output_key);
        }
        return true;
    }
    return false;
}

bool WorkflowCompositionService::ExternalBindingMatches(
    const WorkflowCompositionSpec& composition,
    const WorkflowCompositionNode& node,
    const WorkflowPortDefinition& input,
    std::string* resolved_label_out) const {
    for (const auto& binding : composition.external_inputs) {
        if (binding.node_key != node.node_key || binding.input_key != input.key) {
            continue;
        }
        if (binding.data_kind != input.data_kind || binding.ref_kind != input.ref_kind) {
            return false;
        }
        if (resolved_label_out) {
            *resolved_label_out = "external." + binding.data_kind;
        }
        return true;
    }
    return false;
}

} // namespace savor::db::execution::workflow
