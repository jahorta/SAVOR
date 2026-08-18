#pragma once

#include "../ProgramRuntime/Composition/PredicateComposition.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace savor::runtime::predicates {

inline constexpr std::uint32_t PredicateExecutionPackageWireVersionV1 = 1;

struct PredicateHookPointV1
{
    std::string canonical_id;
    std::uint32_t pc = 0;
    auto operator<=>(const PredicateHookPointV1&) const = default;
};

struct PredicateHookContractV1
{
    std::string canonical_id;
    std::uint32_t revision = 0;
    std::string content_sha256;
    std::vector<PredicateHookPointV1> points;
    auto operator<=>(const PredicateHookContractV1&) const = default;
};

enum class PredicateObservationSourceKindV1 : std::uint8_t
{
    GuestAddress,
    RegisteredQuery,
    HookReceipt,
    RegisteredReducer,
};

enum class PredicateBaselineUpdatePolicyV1 : std::uint8_t
{
    First,
    Latest,
};

enum class PredicateWitnessSourceKindV1 : std::uint8_t
{
    ConcreteValue,
    // The derived-state service captures this value at its registered
    // breakpoint(s). Predicate evaluation reads the latest authoritative
    // snapshot in the current workset item; the evaluation hook does not need
    // to be one of the capture hooks.
    DerivedStateQuery,
    CurrentHookReceipt,
    PinnedGuestMemory,
    BaselineObservation,
};

struct PredicateWitnessSourceBindingV1
{
    std::uint32_t witness_ordinal = 0;
    PredicateWitnessSourceKindV1 source_kind =
        PredicateWitnessSourceKindV1::ConcreteValue;
    program::TypeRef value_type;
    std::optional<program::LiteralValue> concrete_value;
    std::optional<program::ExactDependencyIdentity> source;
    PredicateObservationSourceKindV1 observation_source_kind =
        PredicateObservationSourceKindV1::RegisteredQuery;
    std::string source_field;
    std::optional<std::uint64_t> pinned_guest_address;
    std::string baseline_capture_hook_id;
    PredicateBaselineUpdatePolicyV1 baseline_update_policy =
        PredicateBaselineUpdatePolicyV1::First;
    auto operator<=>(const PredicateWitnessSourceBindingV1&) const = default;
};

struct ResolvedPredicateDefinitionV1
{
    std::int64_t revision_id = 0;
    std::string content_sha256;
    program::composition::PredicateDefinition definition;
    auto operator<=>(const ResolvedPredicateDefinitionV1&) const = default;
};

struct PredicateExecutionBindingV1
{
    std::int64_t execution_binding_revision_id = 0;
    std::string canonical_id;
    std::uint32_t revision = 0;
    std::string content_sha256;
    ResolvedPredicateDefinitionV1 definition;
    std::vector<PredicateWitnessSourceBindingV1> witnesses;
    auto operator<=>(const PredicateExecutionBindingV1&) const = default;
};

enum class PredicateOccurrencePolicyV1 : std::uint8_t
{
    First,
    Every,
    Ordinal,
    GuardOnce,
};

struct PredicateGroupMemberV1
{
    std::uint32_t ordinal = 0;
    std::int64_t execution_binding_revision_id = 0;
    std::vector<std::string> semantic_hook_ids;
    PredicateOccurrencePolicyV1 occurrence = PredicateOccurrencePolicyV1::First;
    std::optional<std::uint32_t> occurrence_ordinal;
    std::optional<std::int64_t> guard_execution_binding_revision_id;
    program::composition::PredicateReaction reaction =
        program::composition::PredicateReaction::RecordAndContinue;
    bool participates_in_aggregation = true;
    bool emit_evidence = false;
    auto operator<=>(const PredicateGroupMemberV1&) const = default;
};

struct ResolvedPredicateGroupV1
{
    std::int64_t predicate_group_revision_id = 0;
    std::string canonical_id;
    std::uint32_t revision = 0;
    std::string content_sha256;
    std::vector<PredicateGroupMemberV1> members;
    auto operator<=>(const ResolvedPredicateGroupV1&) const = default;
};

struct PredicateExecutionPackageV1
{
    PredicateHookContractV1 hook_contract;
    ResolvedPredicateGroupV1 group;
    std::vector<PredicateExecutionBindingV1> execution_bindings;
    std::string content_sha256;
    auto operator<=>(const PredicateExecutionPackageV1&) const = default;
};

struct PredicateExecutionPackageValidationResult
{
    bool ok = false;
    std::string code;
    std::string message;
    explicit operator bool() const noexcept { return ok; }
};

struct PredicateAuthoringSourceV1
{
    std::string display_name;
    std::string category;
    std::string description;
    PredicateObservationSourceKindV1 source_kind =
        PredicateObservationSourceKindV1::RegisteredQuery;
    program::ExactDependencyIdentity identity;
    std::vector<program::TypeRef> argument_types;
    program::TypeRef result_type;
    // Hooks which cause the backing derived-state snapshot to be refreshed.
    std::vector<std::string> capture_hook_ids;
    // Hooks at which the captured value is guaranteed to exist with the
    // freshness required by this source.
    std::vector<std::string> evaluation_hook_ids;
    auto operator<=>(const PredicateAuthoringSourceV1&) const = default;
};

struct PredicateAuthoringReceiptFieldV1
{
    std::string field_name;
    std::string display_name;
    std::string description;
    program::TypeRef value_type;
    auto operator<=>(const PredicateAuthoringReceiptFieldV1&) const = default;
};

struct PredicateAuthoringHookV2
{
    std::string canonical_id;
    std::string display_name;
    std::string description;
    std::uint32_t pc = 0;
    auto operator<=>(const PredicateAuthoringHookV2&) const = default;
};

struct PredicateAuthoringSemanticInputV2
{
    std::string role_id;
    std::string witness_name;
    std::string display_name;
    std::string description;
    program::TypeRef value_type;
    std::optional<program::ExactDependencyIdentity> recommended_query;
    auto operator<=>(const PredicateAuthoringSemanticInputV2&) const = default;
};

struct PredicateAuthoringValueArgumentV2
{
    std::string key;
    std::string display_name;
    std::string description;
    program::TypeRef value_type;
    std::optional<std::string> automatic_semantic_input_role;
    std::optional<std::string> suggested_parameter_name;
    auto operator<=>(const PredicateAuthoringValueArgumentV2&) const = default;
};

struct PredicateAuthoringValueRecipeV2
{
    std::string recipe_id;
    std::string category;
    std::string display_name;
    std::string description;
    program::ExactDependencyIdentity reducer;
    std::vector<PredicateAuthoringValueArgumentV2> arguments;
    program::TypeRef result_type;
    auto operator<=>(const PredicateAuthoringValueRecipeV2&) const = default;
};

enum class PredicateGuidedNodeKindV1 : std::uint8_t;

struct PredicateAuthoringOperatorV2
{
    std::string operator_id;
    std::string display_name;
    std::string description;
    PredicateGuidedNodeKindV1 node_kind;
    bool ordered_only = false;
    auto operator<=>(const PredicateAuthoringOperatorV2&) const = default;
};

struct PredicateAuthoringCatalogV2
{
    std::string canonical_id;
    std::uint32_t revision = 0;
    std::string content_sha256;
    PredicateHookContractV1 hook_contract;
    std::vector<PredicateAuthoringHookV2> hooks;
    std::vector<PredicateAuthoringSourceV1> query_sources;
    std::vector<PredicateAuthoringSourceV1> reducers;
    std::vector<PredicateAuthoringReceiptFieldV1> hook_receipt_fields;
    std::vector<PredicateAuthoringSemanticInputV2> semantic_inputs;
    std::vector<PredicateAuthoringValueRecipeV2> value_recipes;
    std::vector<PredicateAuthoringOperatorV2> comparison_operators;
    std::vector<PredicateAuthoringOperatorV2> calculation_operators;
    std::vector<program::TypeRef> value_types;
    auto operator<=>(const PredicateAuthoringCatalogV2&) const = default;
};

enum class PredicateGuidedNodeKindV1 : std::uint8_t
{
    SemanticInput,
    Parameter,
    Literal,
    CatalogValue,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    All,
    Any,
    Not,
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
};

struct PredicateGuidedNodeV1
{
    PredicateGuidedNodeKindV1 kind = PredicateGuidedNodeKindV1::Literal;
    std::string key;
    std::optional<program::TypeRef> declared_type;
    std::optional<program::LiteralValue> literal;
    std::vector<PredicateGuidedNodeV1> children;
    auto operator<=>(const PredicateGuidedNodeV1&) const = default;
};

struct PredicateAuthoringDiagnosticV1
{
    std::string path;
    std::string code;
    std::string message;
    auto operator<=>(const PredicateAuthoringDiagnosticV1&) const = default;
};

struct PredicateGuidedCompileResultV1
{
    bool ok = false;
    program::composition::PredicateDefinition definition;
    std::vector<PredicateAuthoringDiagnosticV1> diagnostics;
    explicit operator bool() const noexcept { return ok; }
};

struct PredicateGuidedReconstructionResultV1
{
    bool ok = false;
    PredicateGuidedNodeV1 root;
    std::vector<PredicateAuthoringDiagnosticV1> diagnostics;
    explicit operator bool() const noexcept { return ok; }
};

[[nodiscard]] PredicateHookContractV1 BattlePredicateHookContractV1();
[[nodiscard]] PredicateAuthoringCatalogV2 BattlePredicateAuthoringCatalogV2();
[[nodiscard]] PredicateGuidedCompileResultV1 CompileGuidedPredicateDefinitionV1(
    const PredicateGuidedNodeV1& root,
    const PredicateAuthoringCatalogV2& catalog);
[[nodiscard]] PredicateGuidedReconstructionResultV1
ReconstructGuidedPredicateDefinitionV1(
    const program::composition::PredicateDefinition& definition,
    const PredicateAuthoringCatalogV2& catalog);
[[nodiscard]] std::optional<PredicateWitnessSourceBindingV1>
PlanPredicateSemanticWitnessSourceV1(
    const program::composition::PredicateWitness& witness,
    std::uint32_t witness_ordinal,
    const PredicateAuthoringCatalogV2& catalog);
[[nodiscard]] ResolvedPredicateGroupV1 EmptyPredicateGroupV1();
[[nodiscard]] PredicateExecutionPackageV1 EmptyPredicateExecutionPackageV1();

[[nodiscard]] std::string ComputePredicateHookContractHashV1(
    const PredicateHookContractV1& contract);
[[nodiscard]] std::string ComputePredicateDefinitionHashV1(
    const program::composition::PredicateDefinition& definition);
[[nodiscard]] std::string ComputePredicateDefinitionSemanticHashV1(
    const program::composition::PredicateDefinition& definition);
[[nodiscard]] std::string ComputePredicateExecutionBindingHashV1(
    const PredicateExecutionBindingV1& binding);
[[nodiscard]] std::string ComputePredicateExecutionBindingSemanticHashV1(
    const PredicateExecutionBindingV1& binding);
[[nodiscard]] std::string ComputeResolvedPredicateGroupHashV1(
    const ResolvedPredicateGroupV1& group);
[[nodiscard]] std::string ComputeResolvedPredicateGroupSemanticHashV1(
    const ResolvedPredicateGroupV1& group);
[[nodiscard]] std::string ComputePredicateExecutionPackageHashV1(
    const PredicateExecutionPackageV1& package);
[[nodiscard]] std::string ComputePredicateAuthoringCatalogHashV2(
    const PredicateAuthoringCatalogV2& catalog);

[[nodiscard]] PredicateExecutionPackageValidationResult
ValidatePredicateExecutionPackageV1(const PredicateExecutionPackageV1& package);

[[nodiscard]] bool EncodePredicateExecutionPackageV1(
    const PredicateExecutionPackageV1& package,
    std::vector<std::uint8_t>& output,
    std::string* diagnostic = nullptr);
[[nodiscard]] bool DecodePredicateExecutionPackageV1(
    std::span<const std::uint8_t> input,
    PredicateExecutionPackageV1& output,
    std::string* diagnostic = nullptr);

} // namespace savor::runtime::predicates
