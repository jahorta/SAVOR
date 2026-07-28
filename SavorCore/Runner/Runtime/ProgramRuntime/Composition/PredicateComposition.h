#pragma once

#include "CompositionSupport.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::runtime::program::composition {

enum class PredicateWitnessRequirement : std::uint8_t
{
    Required,
    OptionalNotApplicable,
    OptionalUnavailable,
};

struct PredicateWitness
{
    std::string name;
    TypeRef parameter_type;
    TypeRef value_type;
    PredicateWitnessRequirement requirement =
        PredicateWitnessRequirement::Required;

    auto operator<=>(const PredicateWitness&) const = default;
};

enum class PredicateExpressionKind : std::uint8_t
{
    Witness,
    Literal,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    BooleanAnd,
    BooleanOr,
    BooleanNot,
    ImportedReducer,
};

struct PredicateExpressionNode
{
    PredicateExpressionKind kind = PredicateExpressionKind::Witness;
    std::optional<std::size_t> witness_index;
    std::optional<LiteralValue> literal;
    std::vector<std::size_t> operands;
    std::optional<ExactDependencyIdentity> reducer;
    TypeRef result_type = TypeRef::Builtin(BuiltinType::Bool);
    std::string source_label;

    auto operator<=>(const PredicateExpressionNode&) const = default;
};

enum class PredicateUsePolicy : std::uint8_t
{
    Branch,
    ReturnDomainRejection,
    StructuredFail,
    EmitRecord,
    Accumulate,
};

struct PredicateCheck
{
    std::string canonical_id;
    std::string semantic_point_id;
    PredicateUsePolicy use_policy = PredicateUsePolicy::Branch;
    bool emit_condition_observation = false;
    std::string domain_rejection_code;
    std::optional<LiteralValue> domain_rejection;
    std::string structured_failure_code;

    auto operator<=>(const PredicateCheck&) const = default;
};

struct PredicateDefinition
{
    std::string canonical_id;
    std::uint32_t revision = 0;
    std::string source_name;
    std::vector<PredicateWitness> witnesses;
    std::vector<PredicateExpressionNode> expression;
    std::size_t root_expression = 0;
    PredicateCheck check;
    std::optional<TypeRef> domain_outcome_type;
    std::optional<LiteralValue> domain_success;
};

[[nodiscard]] CompositionResult LowerPredicate(
    const PredicateDefinition& definition,
    ProgramModule& module);

} // namespace savor::runtime::program::composition
