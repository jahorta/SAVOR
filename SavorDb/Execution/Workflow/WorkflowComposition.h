#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace savor::db::execution::workflow {

enum class WorkflowPortDirection {
    Input = 0,
    Output = 1,
};

struct WorkflowPortDefinition {
    std::string key;
    std::string data_kind;
    std::string ref_kind;
    std::string display_name;
    bool required = true;
};

enum class WorkflowLaunchArgumentValueType : std::uint8_t {
    Integer = 1,
    Text = 2,
    Boolean = 3,
    Json = 4,
    Choice = 5,
};

enum class WorkflowUnitExecutionShape : std::uint8_t {
    WorkflowInstance = 0,
    WorkflowExpansion = 1,
};

enum class WorkflowExpansionKind : std::uint8_t {
    None = 0,
    TasMovieFirstBattleExploration = 1,
    TasMovieDelayExploration = 2,
};

struct WorkflowLaunchArgumentChoiceDefinition {
    std::string value;
    std::string display_name;
};

struct WorkflowLaunchArgumentDefinition {
    std::string key;
    std::string display_name;
    WorkflowLaunchArgumentValueType value_type = WorkflowLaunchArgumentValueType::Text;
    bool required = false;
    std::optional<std::string> default_value;
    std::optional<std::int64_t> minimum_integer;
    std::optional<std::uint64_t> maximum_integer;
    std::vector<WorkflowLaunchArgumentChoiceDefinition> choices;
};

struct WorkflowLaunchArgumentConstraint {
    std::string lesser_or_equal_key;
    std::string greater_or_equal_key;
    std::string message;
};

struct WorkflowAuthoredRefRequirement {
    std::string ref_kind;
    std::string display_name;
    bool required = true;
};

struct WorkflowUnitStepTemplate {
    std::string step_key_suffix;
    std::string step_kind;
    int priority = 0;
    int max_attempts = 1;
};

struct WorkflowUnitDefinition {
    std::string unit_kind;
    std::string display_name;
    std::string description;
    bool hidden = false;
    bool standalone_launchable = true;
    WorkflowUnitExecutionShape execution_shape =
        WorkflowUnitExecutionShape::WorkflowInstance;
    WorkflowExpansionKind expansion_kind = WorkflowExpansionKind::None;
    std::string standalone_presentation_family_key;
    std::string standalone_presentation_family_display_name;
    std::string unit_variant;
    std::string breakpoint_profile_key;
    std::string default_activation_params_json;
    std::vector<WorkflowAuthoredRefRequirement> authored_refs;
    std::vector<WorkflowPortDefinition> required_inputs;
    std::vector<WorkflowPortDefinition> possible_outputs;
    std::vector<WorkflowLaunchArgumentDefinition> launch_arguments;
    std::vector<WorkflowLaunchArgumentConstraint> launch_argument_constraints;
    std::vector<std::string> internal_step_kinds;
    std::vector<WorkflowUnitStepTemplate> step_templates;
};

struct WorkflowExternalInputBinding {
    std::string node_key;
    std::string input_key;
    std::string data_kind;
    std::string ref_kind;
    std::optional<std::int64_t> ref_id;
};

struct WorkflowStandalonePresentationEntry {
    std::string presentation_key;
    std::string display_name;
    std::string description;
    std::vector<WorkflowUnitDefinition> members;
};

[[nodiscard]] std::vector<WorkflowStandalonePresentationEntry>
BuildStandalonePresentationEntries(
    std::vector<WorkflowUnitDefinition> units);

struct WorkflowUnitOutputBinding {
    std::string from_node_key;
    std::string output_key;
    std::string to_node_key;
    std::string input_key;
    std::optional<std::string> guard_kind;
    std::optional<std::string> guard_value;
};

struct WorkflowCompositionNode {
    std::string node_key;
    std::string unit_kind;
};

struct WorkflowCompositionSpec {
    std::vector<WorkflowCompositionNode> nodes;
    std::vector<WorkflowExternalInputBinding> external_inputs;
    std::vector<WorkflowUnitOutputBinding> output_bindings;
};

struct WorkflowCompositionIssue {
    std::string node_key;
    std::string input_key;
    std::string data_kind;
    std::string message;
};

struct WorkflowCompositionPreviewNode {
    std::string node_key;
    std::string unit_kind;
    std::vector<std::string> resolved_inputs;
    std::vector<std::string> possible_outputs;
};

struct WorkflowCompositionPreview {
    bool valid = false;
    std::vector<WorkflowCompositionPreviewNode> nodes;
    std::vector<WorkflowCompositionIssue> issues;
};

class WorkflowUnitRegistry {
public:
    bool RegisterUnit(WorkflowUnitDefinition definition, std::string* error_out);
    const WorkflowUnitDefinition* Find(std::string_view unit_kind) const;
    std::vector<WorkflowUnitDefinition> ListUnits() const;

private:
    std::unordered_map<std::string, WorkflowUnitDefinition> units_;
};

WorkflowUnitRegistry BuildDefaultWorkflowUnitRegistry();

class WorkflowCompositionService {
public:
    explicit WorkflowCompositionService(const WorkflowUnitRegistry* registry);

    WorkflowCompositionPreview Preview(const WorkflowCompositionSpec& composition) const;
    std::vector<WorkflowUnitDefinition> ListUnitsCompatibleWith(
        const std::vector<WorkflowPortDefinition>& available_outputs) const;

private:
    const WorkflowUnitDefinition* FindNodeUnit(const WorkflowCompositionNode& node) const;
    const WorkflowPortDefinition* FindInput(
        const WorkflowUnitDefinition& unit,
        std::string_view input_key) const;
    const WorkflowPortDefinition* FindOutput(
        const WorkflowUnitDefinition& unit,
        std::string_view output_key) const;
    bool OutputBindingMatches(
        const WorkflowCompositionSpec& composition,
        const WorkflowCompositionNode& node,
        const WorkflowPortDefinition& input,
        std::string* resolved_label_out,
        WorkflowCompositionIssue* issue_out) const;
    bool ExternalBindingMatches(
        const WorkflowCompositionSpec& composition,
        const WorkflowCompositionNode& node,
        const WorkflowPortDefinition& input,
        std::string* resolved_label_out) const;

    const WorkflowUnitRegistry* registry_ = nullptr;
};

} // namespace savor::db::execution::workflow
