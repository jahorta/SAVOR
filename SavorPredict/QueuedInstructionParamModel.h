#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

enum class QueuedInstructionParamKind {
    Unset,
    BasicAttackRoute,
    AbilityId,
    ItemId,
    Unknown,
};

enum class QueuedInstructionParamStage {
    Initialized,
    CommandSelected,
    ExecutionResolved,
};

enum class QueuedInstructionParamStatus {
    Validated,
    Provisional,
    MissingInput,
    Unsupported,
    Inconsistent,
};

enum class QueuedInstructionCommandKind {
    Initialize,
    Attack,
    Magic,
    SMove,
    Crew,
    Item,
    Guard,
    Focus,
    Reset,
    Unknown,
};

enum class BasicAttackExecutionRoute {
    DirectMelee,
    FallbackRanged,
    Unknown,
};

enum class QueuedStdActionState : std::int16_t {
    DirectNoncritical5 = 5,
    DirectCritical6 = 6,
    FallbackRanged7 = 7,
};

struct QueuedInstructionParamValue {
    std::optional<std::int16_t> raw_value;
    QueuedInstructionParamKind kind = QueuedInstructionParamKind::Unknown;
    QueuedInstructionParamStage stage = QueuedInstructionParamStage::Initialized;
    QueuedInstructionParamStatus status = QueuedInstructionParamStatus::MissingInput;
    std::string confidence;
    std::string provenance;
};

struct QueuedInstructionCommandInput {
    QueuedInstructionCommandKind command = QueuedInstructionCommandKind::Unknown;
    std::optional<std::uint16_t> movement_flags;
    std::optional<std::int16_t> selected_id;
};

struct QueuedInstructionCommandResult {
    std::optional<std::int16_t> instruction;
    QueuedInstructionParamValue parameter;
};

struct QueuedInstructionEvidenceInput {
    std::optional<std::int16_t> reliable_pre_macro_value;
    std::optional<std::int16_t> reliable_post_macro_value;
    std::vector<std::int16_t> macro_observed_post_write_values;
    bool macro_capture_complete = false;
};

struct QueuedInstructionEvidenceResult {
    std::optional<std::int16_t> accepted_value;
    QueuedInstructionParamStatus status = QueuedInstructionParamStatus::MissingInput;
    bool macro_observed = false;
    bool macro_agrees_with_post_snapshot = false;
    bool macro_trusted = false;
    std::string provenance;
};

struct BasicAttackQueuedStateInput {
    BasicAttackExecutionRoute route = BasicAttackExecutionRoute::Unknown;
    std::optional<int> attack_result;
    bool counter_follow_up = false;
};

struct BasicAttackQueuedStateResult {
    QueuedInstructionParamStatus status = QueuedInstructionParamStatus::MissingInput;
    std::optional<QueuedStdActionState> queued_state;
    std::optional<std::int16_t> instruction_mode;
    std::string confidence;
    std::string provenance;
};

QueuedInstructionCommandResult model_queued_instruction_command(
    const QueuedInstructionCommandInput& input);

QueuedInstructionEvidenceResult reconcile_queued_instruction_macro_evidence(
    const QueuedInstructionEvidenceInput& input);

BasicAttackExecutionRoute basic_attack_route_from_final_parameter(
    std::optional<std::int16_t> final_parameter);

std::optional<std::int16_t> provisional_instruction_mode_for_route(
    BasicAttackExecutionRoute route);

BasicAttackQueuedStateResult model_basic_attack_queued_state(
    const BasicAttackQueuedStateInput& input);

const char* queued_instruction_param_kind_name(QueuedInstructionParamKind kind);
const char* queued_instruction_param_stage_name(QueuedInstructionParamStage stage);
const char* queued_instruction_param_status_name(QueuedInstructionParamStatus status);
const char* queued_instruction_command_kind_name(QueuedInstructionCommandKind command);
const char* basic_attack_execution_route_name(BasicAttackExecutionRoute route);
const char* queued_std_action_state_name(QueuedStdActionState state);

} // namespace savor::predict
