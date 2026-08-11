#pragma once

#include "../ProgramRuntime/Composition/PredicateComposition.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace savor::runtime::predicates {

inline constexpr std::uint32_t PredicateBundleWireVersionV1 = 1;

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

struct PredicateParameterV1
{
    std::uint32_t ordinal = 0;
    std::string name;
    program::TypeRef value_type;
    auto operator<=>(const PredicateParameterV1&) const = default;
};

struct PredicateObservationV1
{
    std::uint32_t ordinal = 0;
    std::string stable_key;
    std::string semantic_hook_id;
    PredicateObservationSourceKindV1 source_kind =
        PredicateObservationSourceKindV1::RegisteredQuery;
    program::ExactDependencyIdentity source;
    program::TypeRef value_type;
    // Compatibility-pinned guest reads carry their address explicitly.  It is
    // part of the immutable bundle hash rather than hidden in a DB-side key.
    std::optional<std::uint64_t> pinned_guest_address;
    // Hook-receipt observations project this named field.  Registered query
    // actions and reducers consume the complete canonical hook receipt.
    std::string source_field;
    auto operator<=>(const PredicateObservationV1&) const = default;
};

enum class PredicateBaselineUpdatePolicyV1 : std::uint8_t
{
    First,
    Latest,
};

struct PredicateBaselineV1
{
    std::uint32_t ordinal = 0;
    std::string name;
    std::string capture_hook_id;
    std::uint32_t observation_ordinal = 0;
    PredicateBaselineUpdatePolicyV1 update_policy =
        PredicateBaselineUpdatePolicyV1::First;
    auto operator<=>(const PredicateBaselineV1&) const = default;
};

enum class PredicateWitnessSourceKindV1 : std::uint8_t
{
    Observation,
    Baseline,
    Parameter,
    HookReceipt,
    Literal,
};

struct PredicateWitnessBindingV1
{
    std::uint32_t witness_ordinal = 0;
    PredicateWitnessSourceKindV1 source_kind =
        PredicateWitnessSourceKindV1::Observation;
    std::optional<std::uint32_t> source_ordinal;
    std::string source_field;
    program::TypeRef value_type;
    std::optional<program::LiteralValue> literal;
    auto operator<=>(const PredicateWitnessBindingV1&) const = default;
};

enum class PredicateOccurrencePolicyV1 : std::uint8_t
{
    First,
    Every,
    Ordinal,
    GuardOnce,
};

struct ResolvedPredicateDefinitionV1
{
    std::int64_t revision_id = 0;
    program::composition::PredicateDefinition definition;
    auto operator<=>(const ResolvedPredicateDefinitionV1&) const = default;
};

struct PredicateCheckV1
{
    std::uint32_t ordinal = 0;
    std::int64_t predicate_definition_revision_id = 0;
    program::composition::PredicateCheckUse use;
    PredicateOccurrencePolicyV1 occurrence = PredicateOccurrencePolicyV1::First;
    std::optional<std::uint32_t> occurrence_ordinal;
    std::optional<std::int64_t> guard_predicate_definition_revision_id;
    std::vector<PredicateWitnessBindingV1> witnesses;
    auto operator<=>(const PredicateCheckV1&) const = default;
};

struct ResolvedPredicateBundleV1
{
    std::int64_t bundle_revision_id = 0;
    std::string canonical_id;
    std::uint32_t revision = 0;
    std::string content_sha256;
    std::vector<ResolvedPredicateDefinitionV1> definitions;
    std::vector<PredicateParameterV1> parameters;
    std::vector<PredicateObservationV1> observations;
    std::vector<PredicateBaselineV1> baselines;
    std::vector<PredicateCheckV1> checks;
    auto operator<=>(const ResolvedPredicateBundleV1&) const = default;
};

enum class PredicateAggregationKindV1 : std::uint8_t
{
    PassedCount,
};

struct PredicateBundleBindingV1
{
    std::int64_t bundle_revision_id = 0;
    std::string bundle_content_sha256;
    std::vector<program::LiteralValue> parameter_values;
    std::vector<std::uint32_t> active_check_ordinals;
    PredicateAggregationKindV1 aggregation =
        PredicateAggregationKindV1::PassedCount;
    std::string structural_active_check_sha256;
    std::string content_sha256;
    auto operator<=>(const PredicateBundleBindingV1&) const = default;
};

struct PredicateBundleExecutionPackageV1
{
    PredicateHookContractV1 hook_contract;
    ResolvedPredicateBundleV1 bundle;
    PredicateBundleBindingV1 binding;
    auto operator<=>(const PredicateBundleExecutionPackageV1&) const = default;
};

struct PredicateBundleValidationResult
{
    bool ok = false;
    std::string code;
    std::string message;
    explicit operator bool() const noexcept { return ok; }
};

[[nodiscard]] PredicateHookContractV1 BattlePredicateHookContractV1();
[[nodiscard]] ResolvedPredicateBundleV1 EmptyPredicateBundleV1();
[[nodiscard]] PredicateBundleBindingV1 EmptyPredicateBundleBindingV1();

[[nodiscard]] std::string ComputePredicateHookContractHashV1(
    const PredicateHookContractV1& contract);
[[nodiscard]] std::string ComputeResolvedPredicateBundleHashV1(
    const ResolvedPredicateBundleV1& bundle);
[[nodiscard]] std::string ComputePredicateBundleBindingHashV1(
    const PredicateBundleBindingV1& binding);
[[nodiscard]] std::string ComputePredicateActiveCheckSetHashV1(
    std::span<const std::uint32_t> active_check_ordinals);

[[nodiscard]] PredicateBundleValidationResult ValidatePredicateBundlePackageV1(
    const PredicateBundleExecutionPackageV1& package);

[[nodiscard]] bool EncodePredicateBundleExecutionPackageV1(
    const PredicateBundleExecutionPackageV1& package,
    std::vector<std::uint8_t>& output,
    std::string* diagnostic = nullptr);
[[nodiscard]] bool DecodePredicateBundleExecutionPackageV1(
    std::span<const std::uint8_t> input,
    PredicateBundleExecutionPackageV1& output,
    std::string* diagnostic = nullptr);

} // namespace savor::runtime::predicates
