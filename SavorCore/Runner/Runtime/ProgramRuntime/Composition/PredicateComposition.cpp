#include "PredicateComposition.h"

#include <algorithm>
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
    case PredicateExpressionKind::Equal:
        return InstructionOpcode::Equal;
    case PredicateExpressionKind::NotEqual:
        return InstructionOpcode::NotEqual;
    case PredicateExpressionKind::Less:
        return InstructionOpcode::Less;
    case PredicateExpressionKind::LessEqual:
        return InstructionOpcode::LessEqual;
    case PredicateExpressionKind::Greater:
        return InstructionOpcode::Greater;
    case PredicateExpressionKind::GreaterEqual:
        return InstructionOpcode::GreaterEqual;
    case PredicateExpressionKind::BooleanAnd:
        return InstructionOpcode::BooleanAnd;
    case PredicateExpressionKind::BooleanOr:
        return InstructionOpcode::BooleanOr;
    case PredicateExpressionKind::BooleanNot:
        return InstructionOpcode::BooleanNot;
    case PredicateExpressionKind::ImportedReducer:
        return InstructionOpcode::CallReducer;
    case PredicateExpressionKind::Witness:
        return InstructionOpcode::Copy;
    case PredicateExpressionKind::Literal:
        return InstructionOpcode::Constant;
    }
    return InstructionOpcode::Copy;
}

std::optional<CompositionResult> Validate(
    const PredicateDefinition& definition)
{
    if (definition.canonical_id.empty() || definition.revision == 0 ||
        definition.check.canonical_id.empty() ||
        definition.check.semantic_point_id.empty())
    {
        return detail::Fail(
            "predicate.invalid_identity",
            "predicate definition and check require stable identities");
    }
    if (definition.expression.empty() ||
        definition.root_expression >= definition.expression.size())
    {
        return detail::Fail(
            "predicate.invalid_expression",
            "predicate requires a valid finite expression");
    }
    std::set<std::string> witness_names;
    for (const auto& witness : definition.witnesses)
    {
        if (witness.name.empty() ||
            !witness_names.insert(witness.name).second)
        {
            return detail::Fail(
                "predicate.invalid_witness",
                "predicate witness names must be nonempty and unique");
        }
    }
    for (std::size_t index = 0; index < definition.expression.size(); ++index)
    {
        const auto& node = definition.expression[index];
        if (node.kind == PredicateExpressionKind::Witness)
        {
            if (!node.witness_index ||
                *node.witness_index >= definition.witnesses.size() ||
                !node.operands.empty())
            {
                return detail::Fail(
                    "predicate.invalid_witness_expression",
                    "witness expression references an unknown witness");
            }
        }
        else if (node.kind == PredicateExpressionKind::Literal)
        {
            if (!node.literal || !node.operands.empty())
            {
                return detail::Fail(
                    "predicate.invalid_literal",
                    "literal expression requires exactly one literal payload");
            }
        }
        else
        {
            const std::size_t expected =
                node.kind == PredicateExpressionKind::BooleanNot ? 1 : 2;
            if (node.kind == PredicateExpressionKind::ImportedReducer)
            {
                if (!node.reducer || node.operands.empty())
                {
                    return detail::Fail(
                        "predicate.invalid_reducer",
                        "imported predicate reducer requires exact identity and operands");
                }
            }
            else if (node.operands.size() != expected)
            {
                return detail::Fail(
                    "predicate.invalid_arity",
                    "predicate expression has invalid operand arity");
            }
            for (const auto operand : node.operands)
            {
                if (operand >= index)
                {
                    return detail::Fail(
                        "predicate.noncanonical_expression",
                        "predicate nodes must be in deterministic dependency order");
                }
            }
        }
    }
    if (definition.check.use_policy ==
            PredicateUsePolicy::ReturnDomainRejection &&
        (!definition.domain_outcome_type ||
         !definition.domain_success ||
         !definition.check.domain_rejection ||
         definition.check.domain_rejection_code.empty()))
    {
        return detail::Fail(
            "predicate.missing_domain_outcome",
            "domain rejection policy requires typed success and rejection outcomes plus a code");
    }
    if (definition.check.use_policy ==
            PredicateUsePolicy::ReturnDomainRejection &&
        (definition.domain_success->type !=
             *definition.domain_outcome_type ||
         definition.check.domain_rejection->type !=
             *definition.domain_outcome_type))
    {
        return detail::Fail(
            "predicate.domain_outcome_type_mismatch",
            "predicate domain-success and rejection values must match the declared domain outcome type");
    }
    if (definition.check.use_policy !=
            PredicateUsePolicy::ReturnDomainRejection &&
        (definition.domain_outcome_type ||
         definition.domain_success ||
         definition.check.domain_rejection ||
         !definition.check.domain_rejection_code.empty()))
    {
        return detail::Fail(
            "predicate.unused_domain_outcome",
            "domain outcome declarations are valid only for the domain-rejection use policy");
    }
    if (definition.check.use_policy == PredicateUsePolicy::StructuredFail &&
        definition.check.structured_failure_code.empty())
    {
        return detail::Fail(
            "predicate.missing_failure_code",
            "structured-fail policy requires an explicit code");
    }
    return std::nullopt;
}

TypeSchemaDefinition EvaluationSchema(const PredicateDefinition& definition)
{
    const auto identity = ExactSchema(
        definition.canonical_id + ".Evaluation",
        definition.revision,
        "enum{Satisfied=0,Unsatisfied=1,NotApplicable=2,Unavailable=3}");
    return {
        .identity = identity,
        .kind = TypeSchemaKind::ClosedEnum,
        .enum_members = {
            {"Satisfied", 0},
            {"Unsatisfied", 1},
            {"NotApplicable", 2},
            {"Unavailable", 3},
        },
    };
}

std::optional<ProgramValueId> EvaluationConstant(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    const SchemaIdentity& schema,
    std::int64_t value,
    std::string selector)
{
    const TypeRef type = TypeRef::Named(schema);
    return builder.AddInstruction(
        function,
        block,
        InstructionOpcode::Constant,
        type,
        {},
        {},
        std::move(selector),
        LiteralValue{
            .type = type,
            .payload = EnumValue{.schema = schema, .value = value},
        });
}

void AddEmission(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    ProgramValueId evaluation,
    const PredicateDefinition& definition,
    std::string status)
{
    if (!definition.check.emit_condition_observation &&
        definition.check.use_policy != PredicateUsePolicy::EmitRecord)
    {
        return;
    }
    (void)builder.AddInstruction(
        function,
        block,
        InstructionOpcode::EmitRecord,
        std::nullopt,
        std::array{evaluation},
        {},
        std::format(
            "condition-observation/{}/{}/{}",
            definition.check.canonical_id,
            definition.check.semantic_point_id,
            status));
}

} // namespace

CompositionResult LowerPredicate(
    const PredicateDefinition& definition,
    ProgramModule& module)
{
    if (const auto invalid = Validate(definition))
        return *invalid;

    ProgramModule candidate = module;
    ModuleFragmentBuilder builder(
        candidate,
        definition.source_name.empty()
            ? definition.canonical_id
            : definition.source_name,
        std::format(
            "predicate/{}/{}",
            definition.canonical_id,
            definition.revision));

    auto evaluation_schema = EvaluationSchema(definition);
    const auto evaluation_identity = evaluation_schema.identity;
    builder.AddLocalType(std::move(evaluation_schema));
    for (const auto& witness : definition.witnesses)
    {
        builder.AddTypeImport(witness.parameter_type);
        builder.AddTypeImport(witness.value_type);
    }
    for (const auto& expression : definition.expression)
        builder.AddTypeImport(expression.result_type);
    if (definition.domain_outcome_type)
        builder.AddTypeImport(*definition.domain_outcome_type);

    std::vector<ValueDefinition> arguments;
    arguments.reserve(definition.witnesses.size());
    for (const auto& witness : definition.witnesses)
        arguments.push_back(builder.NewArgument(witness.parameter_type));

    auto& function = builder.AddFunction(
        "predicate." + definition.canonical_id,
        arguments,
        TypeRef::Named(evaluation_identity),
        definition.domain_outcome_type);
    function.blocks.reserve(definition.witnesses.size() + 7);
    auto& entry = builder.AddBlock(function);

    std::vector<ProgramBlockId> availability_blocks;
    availability_blocks.reserve(definition.witnesses.size() + 1);
    availability_blocks.push_back(entry.id);
    for (std::size_t index = 0; index < definition.witnesses.size(); ++index)
    {
        if (definition.witnesses[index].requirement !=
            PredicateWitnessRequirement::Required ||
            definition.witnesses[index].parameter_type !=
                definition.witnesses[index].value_type)
        {
            auto& block = builder.AddBlock(function);
            availability_blocks.push_back(block.id);
        }
    }
    auto& evaluation_block = builder.AddBlock(function);
    auto& satisfied_block = builder.AddBlock(function);
    auto& unsatisfied_block = builder.AddBlock(function);
    auto& not_applicable_block = builder.AddBlock(function);
    auto& unavailable_block = builder.AddBlock(function);
    auto& required_missing_block = builder.AddBlock(function);

    BasicBlock* current = &entry;
    std::size_t availability_index = 1;
    for (std::size_t index = 0; index < definition.witnesses.size(); ++index)
    {
        const auto& witness = definition.witnesses[index];
        const bool optional_parameter =
            witness.parameter_type != witness.value_type;
        if (!optional_parameter)
            continue;

        const auto present = builder.AddInstruction(
            function,
            *current,
            InstructionOpcode::OptionalIsPresent,
            TypeRef::Builtin(BuiltinType::Bool),
            std::array{arguments[index].id},
            {},
            "witness/" + witness.name + "/present");
        const ProgramBlockId present_target =
            availability_index < availability_blocks.size()
            ? availability_blocks[availability_index++]
            : evaluation_block.id;
        ProgramBlockId absent_target = required_missing_block.id;
        if (witness.requirement ==
            PredicateWitnessRequirement::OptionalNotApplicable)
        {
            absent_target = not_applicable_block.id;
        }
        else if (witness.requirement ==
            PredicateWitnessRequirement::OptionalUnavailable)
        {
            absent_target = unavailable_block.id;
        }
        builder.SetTerminator(
            function,
            *current,
            Terminator{
                .kind = TerminatorKind::ConditionalBranch,
                .condition_or_selector = present,
                .edges = {
                    {.target = present_target},
                    {.target = absent_target},
                },
            },
            "witness/" + witness.name + "/availability");
        current = &*std::ranges::find(
            function.blocks,
            present_target,
            &BasicBlock::id);
    }
    if (current->id != evaluation_block.id)
    {
        builder.SetTerminator(
            function,
            *current,
            Terminator{
                .kind = TerminatorKind::Branch,
                .edges = {{.target = evaluation_block.id}},
            },
            "evaluate");
    }

    std::vector<ProgramValueId> witness_values;
    witness_values.reserve(definition.witnesses.size());
    for (std::size_t index = 0; index < definition.witnesses.size(); ++index)
    {
        const auto& witness = definition.witnesses[index];
        if (witness.parameter_type == witness.value_type)
        {
            witness_values.push_back(arguments[index].id);
        }
        else
        {
            const auto extracted = builder.AddInstruction(
                function,
                evaluation_block,
                InstructionOpcode::OptionalExtract,
                witness.value_type,
                std::array{arguments[index].id},
                {},
                "witness/" + witness.name + "/extract-after-guard");
            witness_values.push_back(*extracted);
        }
    }

    std::vector<ProgramValueId> expression_values;
    expression_values.reserve(definition.expression.size());
    for (std::size_t index = 0; index < definition.expression.size(); ++index)
    {
        const auto& node = definition.expression[index];
        std::optional<ProgramValueId> value;
        if (node.kind == PredicateExpressionKind::Witness)
        {
            value = builder.AddInstruction(
                function,
                evaluation_block,
                InstructionOpcode::Copy,
                node.result_type,
                std::array{witness_values[*node.witness_index]},
                {},
                "expression/" + node.source_label + "/witness");
        }
        else if (node.kind == PredicateExpressionKind::Literal)
        {
            value = builder.AddInstruction(
                function,
                evaluation_block,
                InstructionOpcode::Constant,
                node.result_type,
                {},
                {},
                "expression/" + node.source_label + "/literal",
                node.literal);
        }
        else
        {
            std::vector<ProgramValueId> operands;
            operands.reserve(node.operands.size());
            for (const auto operand : node.operands)
                operands.push_back(expression_values[operand]);
            InstructionTarget target{};
            if (node.kind == PredicateExpressionKind::ImportedReducer)
            {
                builder.AddReducerImport(*node.reducer);
                target = {
                    .kind = InstructionTargetKind::Reducer,
                    .dependency = *node.reducer,
                };
            }
            value = builder.AddInstruction(
                function,
                evaluation_block,
                OpcodeFor(node.kind),
                node.result_type,
                operands,
                std::move(target),
                "expression/" + node.source_label);
        }
        expression_values.push_back(*value);
    }

    builder.SetTerminator(
        function,
        evaluation_block,
        Terminator{
            .kind = TerminatorKind::ConditionalBranch,
            .condition_or_selector =
                expression_values[definition.root_expression],
            .edges = {
                {.target = satisfied_block.id},
                {.target = unsatisfied_block.id},
            },
        },
        "check/branch");

    const auto satisfied = EvaluationConstant(
        builder,
        function,
        satisfied_block,
        evaluation_identity,
        0,
        "evaluation/satisfied");
    AddEmission(
        builder,
        function,
        satisfied_block,
        *satisfied,
        definition,
        "satisfied");
    std::optional<ProgramValueId> satisfied_domain;
    if (definition.domain_outcome_type)
    {
        satisfied_domain = builder.AddInstruction(
            function,
            satisfied_block,
            InstructionOpcode::Constant,
            *definition.domain_outcome_type,
            {},
            {},
            "domain-success/satisfied",
            definition.domain_success);
    }
    builder.SetTerminator(
        function,
        satisfied_block,
        Terminator{
            .kind = TerminatorKind::Return,
            .return_value = satisfied,
            .domain_outcome = satisfied_domain,
        },
        "return/satisfied");

    const auto unsatisfied = EvaluationConstant(
        builder,
        function,
        unsatisfied_block,
        evaluation_identity,
        1,
        "evaluation/unsatisfied");
    AddEmission(
        builder,
        function,
        unsatisfied_block,
        *unsatisfied,
        definition,
        "unsatisfied");
    if (definition.check.use_policy == PredicateUsePolicy::StructuredFail)
    {
        builder.SetTerminator(
            function,
            unsatisfied_block,
            Terminator{
                .kind = TerminatorKind::StructuredFail,
                .failure = StructuredFailure{
                    .code = definition.check.structured_failure_code,
                    .message = "predicate check was unsatisfied",
                    .details = unsatisfied,
                },
            },
            "fail/unsatisfied");
    }
    else
    {
        Terminator terminal{
            .kind = TerminatorKind::Return,
            .return_value = unsatisfied,
        };
        if (definition.check.use_policy ==
            PredicateUsePolicy::ReturnDomainRejection)
        {
            const auto rejection = builder.AddInstruction(
                function,
                unsatisfied_block,
                InstructionOpcode::Constant,
                *definition.domain_outcome_type,
                {},
                {},
                "domain-rejection/" +
                    definition.check.domain_rejection_code,
                definition.check.domain_rejection);
            terminal.domain_outcome = rejection;
        }
        builder.SetTerminator(
            function,
            unsatisfied_block,
            std::move(terminal),
            definition.check.use_policy ==
                    PredicateUsePolicy::ReturnDomainRejection
                ? "return/domain-rejection/" +
                    definition.check.domain_rejection_code
                : "return/unsatisfied");
    }

    const auto not_applicable = EvaluationConstant(
        builder,
        function,
        not_applicable_block,
        evaluation_identity,
        2,
        "evaluation/not-applicable");
    AddEmission(
        builder,
        function,
        not_applicable_block,
        *not_applicable,
        definition,
        "not-applicable");
    std::optional<ProgramValueId> not_applicable_domain;
    if (definition.domain_outcome_type)
    {
        not_applicable_domain = builder.AddInstruction(
            function,
            not_applicable_block,
            InstructionOpcode::Constant,
            *definition.domain_outcome_type,
            {},
            {},
            "domain-success/not-applicable",
            definition.domain_success);
    }
    builder.SetTerminator(
        function,
        not_applicable_block,
        Terminator{
            .kind = TerminatorKind::Return,
            .return_value = not_applicable,
            .domain_outcome = not_applicable_domain,
        },
        "return/not-applicable");

    const auto unavailable = EvaluationConstant(
        builder,
        function,
        unavailable_block,
        evaluation_identity,
        3,
        "evaluation/unavailable");
    AddEmission(
        builder,
        function,
        unavailable_block,
        *unavailable,
        definition,
        "unavailable");
    std::optional<ProgramValueId> unavailable_domain;
    if (definition.domain_outcome_type)
    {
        unavailable_domain = builder.AddInstruction(
            function,
            unavailable_block,
            InstructionOpcode::Constant,
            *definition.domain_outcome_type,
            {},
            {},
            "domain-success/unavailable",
            definition.domain_success);
    }
    builder.SetTerminator(
        function,
        unavailable_block,
        Terminator{
            .kind = TerminatorKind::Return,
            .return_value = unavailable,
            .domain_outcome = unavailable_domain,
        },
        "return/unavailable");

    builder.SetTerminator(
        function,
        required_missing_block,
        Terminator{
            .kind = TerminatorKind::StructuredFail,
            .failure = StructuredFailure{
                .code = "predicate.required_evidence_unavailable",
                .message =
                    "required predicate evidence was unavailable",
            },
        },
        "fail/required-evidence-unavailable");

    const auto function_id = function.id;
    module = std::move(candidate);
    return {
        .ok = true,
        .function = function_id,
    };
}

} // namespace savor::runtime::program::composition
