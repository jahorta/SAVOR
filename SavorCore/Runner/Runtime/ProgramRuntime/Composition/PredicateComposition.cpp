#include "PredicateComposition.h"

#include <array>
#include <format>
#include <set>
#include <utility>

namespace savor::runtime::program::composition {
namespace {

using detail::ModuleFragmentBuilder;

InstructionOpcode OpcodeFor(PredicateExpressionKind kind)
{
    switch (kind)
    {
    case PredicateExpressionKind::Equal: return InstructionOpcode::Equal;
    case PredicateExpressionKind::NotEqual: return InstructionOpcode::NotEqual;
    case PredicateExpressionKind::Less: return InstructionOpcode::Less;
    case PredicateExpressionKind::LessEqual: return InstructionOpcode::LessEqual;
    case PredicateExpressionKind::Greater: return InstructionOpcode::Greater;
    case PredicateExpressionKind::GreaterEqual: return InstructionOpcode::GreaterEqual;
    case PredicateExpressionKind::BooleanAnd: return InstructionOpcode::BooleanAnd;
    case PredicateExpressionKind::BooleanOr: return InstructionOpcode::BooleanOr;
    case PredicateExpressionKind::BooleanNot: return InstructionOpcode::BooleanNot;
    case PredicateExpressionKind::Add: return InstructionOpcode::AddChecked;
    case PredicateExpressionKind::Subtract: return InstructionOpcode::SubtractChecked;
    case PredicateExpressionKind::Multiply: return InstructionOpcode::MultiplyChecked;
    case PredicateExpressionKind::Divide: return InstructionOpcode::DivideChecked;
    case PredicateExpressionKind::Remainder: return InstructionOpcode::RemainderChecked;
    case PredicateExpressionKind::ImportedReducer: return InstructionOpcode::CallReducer;
    case PredicateExpressionKind::Witness: return InstructionOpcode::Copy;
    case PredicateExpressionKind::Literal: return InstructionOpcode::Constant;
    }
    return InstructionOpcode::Copy;
}

bool IsBool(const TypeRef& type)
{
    return type == TypeRef::Builtin(BuiltinType::Bool);
}

bool IsNumeric(const TypeRef& type)
{
    if (type.is_named()) return false;
    switch (type.builtin)
    {
    case BuiltinType::U8:
    case BuiltinType::U16:
    case BuiltinType::U32:
    case BuiltinType::U64:
    case BuiltinType::I32:
    case BuiltinType::I64:
        return true;
    default:
        return false;
    }
}

std::optional<CompositionResult> Validate(
    const PredicateDefinition& definition,
    const PredicateEvaluationPolicy& policy)
{
    if (definition.canonical_id.empty() || definition.revision == 0 ||
        policy.predicate_group_revision_id <= 0 ||
        policy.execution_binding_revision_id <= 0 ||
        policy.semantic_point_id.empty())
        return detail::Fail("predicate.invalid_identity", "predicate definition and group membership require stable identities");
    if (definition.expression.empty() || definition.root_expression >= definition.expression.size())
        return detail::Fail("predicate.invalid_expression", "predicate requires a valid finite expression");
    std::set<std::string> names;
    for (const auto& witness : definition.witnesses)
        if (witness.name.empty() || !names.insert(witness.name).second)
            return detail::Fail("predicate.invalid_witness", "predicate witness names must be nonempty and unique");

    for (std::size_t index = 0; index < definition.expression.size(); ++index)
    {
        const auto& node = definition.expression[index];
        for (const auto operand : node.operands)
            if (operand >= index)
                return detail::Fail("predicate.noncanonical_expression", "predicate nodes must be in deterministic dependency order");
        if (node.kind == PredicateExpressionKind::Witness)
        {
            if (!node.witness_index || *node.witness_index >= definition.witnesses.size()
                || !node.operands.empty() || node.result_type != definition.witnesses[*node.witness_index].value_type)
                return detail::Fail("predicate.invalid_witness_expression", "witness expression does not match an exact required witness");
            continue;
        }
        if (node.kind == PredicateExpressionKind::Literal)
        {
            if (!node.literal || !node.operands.empty() || node.literal->type != node.result_type)
                return detail::Fail("predicate.invalid_literal", "literal expression requires one exactly typed literal");
            continue;
        }
        if (node.kind == PredicateExpressionKind::ImportedReducer)
        {
            if (!node.reducer || node.operands.empty())
                return detail::Fail("predicate.invalid_reducer", "imported reducer requires exact identity and operands");
            continue;
        }
        const std::size_t arity = node.kind == PredicateExpressionKind::BooleanNot ? 1 : 2;
        if (node.operands.size() != arity)
            return detail::Fail("predicate.invalid_arity", "predicate expression has invalid operand arity");
        const auto operand_type = [&](std::size_t position) -> const TypeRef& {
            return definition.expression[node.operands[position]].result_type;
        };
        const bool comparison = node.kind >= PredicateExpressionKind::Equal
            && node.kind <= PredicateExpressionKind::GreaterEqual;
        const bool boolean = node.kind >= PredicateExpressionKind::BooleanAnd
            && node.kind <= PredicateExpressionKind::BooleanNot;
        const bool arithmetic = node.kind >= PredicateExpressionKind::Add
            && node.kind <= PredicateExpressionKind::Remainder;
        if (comparison && (!IsBool(node.result_type) || operand_type(0) != operand_type(1)))
            return detail::Fail("predicate.type_mismatch", "comparison operands must have one exact type and return bool");
        if (boolean && (!IsBool(node.result_type) || !IsBool(operand_type(0))
            || (arity == 2 && !IsBool(operand_type(1)))))
            return detail::Fail("predicate.type_mismatch", "Boolean expressions require Boolean operands and result");
        if (arithmetic && (!IsNumeric(node.result_type) || operand_type(0) != node.result_type
            || operand_type(1) != node.result_type))
            return detail::Fail("predicate.type_mismatch", "arithmetic expressions require one exact numeric type");
    }
    if (!IsBool(definition.expression[definition.root_expression].result_type))
        return detail::Fail("predicate.non_boolean_root", "predicate root expression must be bool");
    return std::nullopt;
}

TypeSchemaDefinition EvaluationSchema(const PredicateDefinition& definition)
{
    return {
        .identity = ExactSchema(
            definition.canonical_id + ".Evaluation", definition.revision,
            "enum PredicateEvaluation{Passed=0,Failed=1}"),
        .kind = TypeSchemaKind::ClosedEnum,
        .enum_members = {{"Passed", 0}, {"Failed", 1}},
    };
}

std::optional<ProgramValueId> EvaluationConstant(
    ModuleFragmentBuilder& builder, ProgramFunction& function, BasicBlock& block,
    const SchemaIdentity& schema, std::int64_t value, std::string selector)
{
    const TypeRef type = TypeRef::Named(schema);
    return builder.AddInstruction(function, block, InstructionOpcode::Constant,
        type, {}, {}, std::move(selector), LiteralValue{
            .type = type,
            .payload = EnumValue{.schema = schema, .value = value},
        });
}

void EmitEvidence(ModuleFragmentBuilder& builder, ProgramFunction& function,
                  BasicBlock& block, ProgramValueId evaluation,
                  const PredicateEvaluationPolicy& policy,
                  const PredicateDefinition& definition,
                  std::string_view status)
{
    if (!policy.emit_evidence) return;
    (void)builder.AddInstruction(function, block, InstructionOpcode::EmitRecord,
        std::nullopt, std::array{evaluation}, {}, std::format(
            "predicate-evidence/definition/{}/{}/binding/{}/group/{}/member/{}/hook/{}/{}",
            definition.canonical_id, definition.revision,
            policy.execution_binding_revision_id,
            policy.predicate_group_revision_id, policy.member_ordinal,
            policy.semantic_point_id, status));
}

} // namespace

CompositionResult LowerPredicate(
    const PredicateDefinition& definition,
    const PredicateEvaluationPolicy& policy,
    ProgramModule& module)
{
    if (const auto invalid = Validate(definition, policy)) return *invalid;
    ProgramModule candidate = module;
    ModuleFragmentBuilder builder(candidate,
        definition.source_name.empty() ? definition.canonical_id : definition.source_name,
        std::format("predicate/{}/{}", definition.canonical_id, definition.revision));
    auto evaluation_schema = EvaluationSchema(definition);
    const auto evaluation_identity = evaluation_schema.identity;
    builder.AddLocalType(std::move(evaluation_schema));
    for (const auto& witness : definition.witnesses) builder.AddTypeImport(witness.value_type);
    for (const auto& expression : definition.expression) builder.AddTypeImport(expression.result_type);

    std::vector<ValueDefinition> arguments;
    for (const auto& witness : definition.witnesses)
        arguments.push_back(builder.NewArgument(witness.value_type));
    auto& function = builder.AddFunction(std::format(
        "predicate.group{}.member{}.{}", policy.predicate_group_revision_id,
        policy.member_ordinal, policy.semantic_point_id),
        arguments, TypeRef::Named(evaluation_identity));
    function.blocks.reserve(3);
    auto& evaluation = builder.AddBlock(function);
    auto& passed_block = builder.AddBlock(function);
    auto& failed_block = builder.AddBlock(function);

    std::vector<ProgramValueId> values;
    values.reserve(definition.expression.size());
    for (const auto& node : definition.expression)
    {
        std::optional<ProgramValueId> value;
        if (node.kind == PredicateExpressionKind::Witness)
            value = builder.AddInstruction(function, evaluation, InstructionOpcode::Copy,
                node.result_type, std::array{arguments[*node.witness_index].id}, {},
                "expression/" + node.source_label + "/witness");
        else if (node.kind == PredicateExpressionKind::Literal)
            value = builder.AddInstruction(function, evaluation, InstructionOpcode::Constant,
                node.result_type, {}, {}, "expression/" + node.source_label + "/literal", node.literal);
        else
        {
            std::vector<ProgramValueId> operands;
            for (const auto operand : node.operands) operands.push_back(values[operand]);
            InstructionTarget target{};
            if (node.kind == PredicateExpressionKind::ImportedReducer)
            {
                builder.AddReducerImport(*node.reducer);
                target = {.kind = InstructionTargetKind::Reducer, .dependency = *node.reducer};
            }
            value = builder.AddInstruction(function, evaluation, OpcodeFor(node.kind),
                node.result_type, operands, std::move(target), "expression/" + node.source_label);
        }
        if (!value) return detail::Fail("predicate.lowering_failed", "predicate expression could not be lowered");
        values.push_back(*value);
    }
    builder.SetTerminator(function, evaluation, {
        .kind = TerminatorKind::ConditionalBranch,
        .condition_or_selector = values[definition.root_expression],
        .edges = {{.target = passed_block.id}, {.target = failed_block.id}},
    }, "check/branch");

    const auto passed = EvaluationConstant(builder, function, passed_block,
        evaluation_identity, 0, "evaluation/passed");
    EmitEvidence(builder, function, passed_block, *passed, policy, definition, "passed");
    builder.SetTerminator(function, passed_block, {
        .kind = TerminatorKind::Return,
        .return_value = passed,
    }, "return/passed");

    const auto failed = EvaluationConstant(builder, function, failed_block,
        evaluation_identity, 1, "evaluation/failed");
    EmitEvidence(builder, function, failed_block, *failed, policy, definition, "failed");
    builder.SetTerminator(function, failed_block, {
        .kind = TerminatorKind::Return,
        .return_value = failed,
    }, policy.reaction == PredicateReaction::AbortOnFail
        ? "return/failed-abort-on-fail" : "return/failed");

    const auto function_id = function.id;
    module = std::move(candidate);
    return {.ok = true, .function = function_id};
}

} // namespace savor::runtime::program::composition
