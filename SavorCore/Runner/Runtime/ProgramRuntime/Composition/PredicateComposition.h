#pragma once

#include "CompositionSupport.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::runtime::program::composition {

struct PredicateWitness
{
    std::string name;
    TypeRef value_type;

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
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
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

// A definition is deliberately pure. It knows nothing about hooks, evidence
// retention, aggregation, or Battle outcomes.
struct PredicateDefinition
{
    std::string canonical_id;
    std::uint32_t revision = 0;
    std::string source_name;
    std::vector<PredicateWitness> witnesses;
    std::vector<PredicateExpressionNode> expression;
    std::size_t root_expression = 0;
};

enum class PredicateReaction : std::uint8_t
{
    RecordAndContinue = 0,
    AbortOnFail,
};

struct PredicateCheckUse
{
    std::string canonical_id;
    std::string semantic_point_id;
    PredicateReaction reaction = PredicateReaction::RecordAndContinue;
    bool emit_evidence = false;
    bool participates_in_aggregation = true;

    auto operator<=>(const PredicateCheckUse&) const = default;
};

[[nodiscard]] CompositionResult LowerPredicate(
    const PredicateDefinition& definition,
    const PredicateCheckUse& check,
    ProgramModule& module);

} // namespace savor::runtime::program::composition
