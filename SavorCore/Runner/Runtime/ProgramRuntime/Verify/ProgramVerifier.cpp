#include "ProgramVerifier.h"

#include "Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace savor::runtime::program {
namespace {

using DiagnosticList = std::vector<VerificationDiagnostic>;

void Add(
    DiagnosticList& diagnostics,
    VerificationErrorCode code,
    std::string message,
    std::optional<ProgramSourceLocationId> source = std::nullopt)
{
    diagnostics.push_back(
        {code, std::move(message), std::move(source)});
}

template <typename T>
bool InsertUnique(std::set<T>& values, const T& value)
{
    return values.emplace(value).second;
}

bool IsBool(const TypeRef& type)
{
    return !type.is_named() && type.builtin == BuiltinType::Bool;
}

bool IsUnit(const TypeRef& type)
{
    return !type.is_named() && type.builtin == BuiltinType::Unit;
}

bool IsInteger(BuiltinType type)
{
    switch (type)
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

bool IsUnsignedInteger(BuiltinType type)
{
    switch (type)
    {
    case BuiltinType::U8:
    case BuiltinType::U16:
    case BuiltinType::U32:
    case BuiltinType::U64:
        return true;
    default:
        return false;
    }
}

bool IsNumeric(const TypeRef& type)
{
    if (type.is_named())
        return false;
    return IsInteger(type.builtin) ||
        type.builtin == BuiltinType::F32 ||
        type.builtin == BuiltinType::F64;
}

bool Contains(
    const std::vector<ExactDependencyIdentity>& values,
    const ExactDependencyIdentity& value)
{
    return std::ranges::find(values, value) != values.end();
}

bool Contains(
    const std::vector<SchemaIdentity>& values,
    const SchemaIdentity& value)
{
    return std::ranges::find(values, value) != values.end();
}

bool Contains(
    const std::vector<CapabilityPackIdentity>& values,
    const CapabilityPackIdentity& value)
{
    return std::ranges::find(values, value) != values.end();
}

bool ContainsPolicy(
    const std::vector<InvocationStatePolicy>& values,
    InvocationStatePolicy value)
{
    return std::ranges::find(values, value) != values.end();
}

bool ContainsIntent(
    const std::vector<ExecutionIntent>& values,
    ExecutionIntent value)
{
    return std::ranges::find(values, value) != values.end();
}

bool PolicySubset(
    const ProgramPolicySet& child,
    const ProgramPolicySet& parent)
{
    for (InvocationStatePolicy value : child.state_policies)
    {
        if (!ContainsPolicy(parent.state_policies, value))
            return false;
    }
    for (ExecutionIntent value : child.execution_intents)
    {
        if (!ContainsIntent(parent.execution_intents, value))
            return false;
    }
    return (!child.permits_movie_playback ||
            parent.permits_movie_playback) &&
        (!child.permits_movie_recording ||
         parent.permits_movie_recording) &&
        (!child.permits_capture || parent.permits_capture) &&
        (!child.permits_replay || parent.permits_replay) &&
        (!child.permits_visual_debug ||
         parent.permits_visual_debug) &&
        (!child.permits_state_replacement ||
         parent.permits_state_replacement) &&
        (!child.permits_resource_promotion ||
         parent.permits_resource_promotion);
}

bool BudgetsValid(const ProgramBudgets& budgets)
{
    return budgets.maximum_instructions != 0 &&
        budgets.maximum_calls != 0 &&
        budgets.maximum_call_depth != 0 &&
        budgets.maximum_action_requests != 0 &&
        budgets.maximum_emissions != 0 &&
        budgets.maximum_artifacts != 0 &&
        budgets.maximum_values != 0 &&
        budgets.maximum_value_bytes != 0 &&
        budgets.maximum_trace_events != 0;
}

bool BudgetsNarrow(
    const ProgramBudgets& child,
    const ProgramBudgets& parent)
{
    return BudgetsValid(child) &&
        child.maximum_instructions <= parent.maximum_instructions &&
        child.maximum_calls <= parent.maximum_calls &&
        child.maximum_call_depth <= parent.maximum_call_depth &&
        child.maximum_action_requests <= parent.maximum_action_requests &&
        child.maximum_emissions <= parent.maximum_emissions &&
        child.maximum_artifacts <= parent.maximum_artifacts &&
        child.maximum_values <= parent.maximum_values &&
        child.maximum_value_bytes <= parent.maximum_value_bytes &&
        child.maximum_trace_events <= parent.maximum_trace_events;
}

struct SchemaLookup
{
    const ProgramModule& module;
    const TypeSchemaRegistry& registry;

    [[nodiscard]] const TypeSchemaDefinition* Resolve(
        const SchemaIdentity& identity) const
    {
        for (const TypeSchemaDefinition& local : module.local_types)
        {
            if (local.identity == identity)
                return &local;
        }
        return Contains(module.type_imports, identity)
            ? registry.Resolve(identity)
            : nullptr;
    }

    [[nodiscard]] bool Resolve(const TypeRef& type) const
    {
        return !type.is_named() || Resolve(*type.named) != nullptr;
    }
};

struct ValueDefinitionSite
{
    TypeRef type;
    std::optional<ProgramBlockId> block;
    std::size_t instruction_index = 0;
    bool block_argument = false;
    bool function_argument = false;
};

struct FunctionContext
{
    const ProgramModule& module;
    const ProgramFunction& function;
    const SchemaLookup& schemas;
    const ActionRegistry& actions;
    const ProgramDefinitionStore& modules;
    DiagnosticList& diagnostics;
    std::map<ProgramBlockId, const BasicBlock*> blocks;
    std::map<ProgramValueId, ValueDefinitionSite> values;
    std::map<ProgramBlockId, std::set<ProgramBlockId>> dominators;
    std::set<ProgramBlockId> reachable;
};

std::vector<BlockEdge> Edges(const Terminator& terminator)
{
    std::vector<BlockEdge> result = terminator.edges;
    for (const EnumSwitchCase& item : terminator.enum_cases)
        result.push_back(item.edge);
    if (terminator.default_edge)
        result.push_back(*terminator.default_edge);
    return result;
}

const ValueDefinitionSite* Lookup(
    const FunctionContext& context,
    ProgramValueId id,
    ProgramSourceLocationId source)
{
    const auto found = context.values.find(id);
    if (found == context.values.end())
    {
        Add(
            context.diagnostics,
            VerificationErrorCode::UndefinedValue,
            "Instruction references an undefined value",
            source);
        return nullptr;
    }
    return &found->second;
}

bool Dominates(
    const FunctionContext& context,
    const ValueDefinitionSite& definition,
    ProgramBlockId use_block,
    std::size_t use_index)
{
    if (definition.function_argument)
        return true;
    if (!definition.block)
        return false;
    if (*definition.block == use_block)
    {
        return definition.block_argument ||
            definition.instruction_index < use_index;
    }
    if (!context.reachable.contains(use_block))
        return false;
    const auto found = context.dominators.find(use_block);
    return found != context.dominators.end() &&
        found->second.contains(*definition.block);
}

const TypeRef* Use(
    const FunctionContext& context,
    ProgramValueId id,
    ProgramBlockId block,
    std::size_t index,
    ProgramSourceLocationId source)
{
    const ValueDefinitionSite* definition =
        Lookup(context, id, source);
    if (!definition)
        return nullptr;
    if (!Dominates(context, *definition, block, index))
    {
        Add(
            context.diagnostics,
            VerificationErrorCode::DefinitionDoesNotDominateUse,
            "Value definition does not dominate its use",
            source);
        return nullptr;
    }
    return &definition->type;
}

bool Same(
    const TypeRef* actual,
    const TypeRef& expected,
    DiagnosticList& diagnostics,
    ProgramSourceLocationId source,
    std::string_view label)
{
    if (!actual)
        return false;
    if (*actual != expected)
    {
        Add(
            diagnostics,
            VerificationErrorCode::TypeMismatch,
            std::string(label) + " has the wrong type",
            source);
        return false;
    }
    return true;
}

const TypeSchemaDefinition* NamedSchema(
    const SchemaLookup& schemas,
    const TypeRef* type,
    TypeSchemaKind kind)
{
    if (!type || !type->is_named())
        return nullptr;
    const TypeSchemaDefinition* schema =
        schemas.Resolve(*type->named);
    return schema && schema->kind == kind ? schema : nullptr;
}

bool ValidateLiteral(
    const LiteralValue& literal,
    const SchemaLookup& schemas)
{
    if (!literal.type.is_named())
    {
        switch (literal.type.builtin)
        {
        case BuiltinType::Unit:
            return std::holds_alternative<UnitValue>(literal.payload);
        case BuiltinType::Bool:
            return std::holds_alternative<bool>(literal.payload);
        case BuiltinType::U8:
            return std::holds_alternative<std::uint8_t>(literal.payload);
        case BuiltinType::U16:
            return std::holds_alternative<std::uint16_t>(literal.payload);
        case BuiltinType::U32:
            return std::holds_alternative<std::uint32_t>(literal.payload);
        case BuiltinType::U64:
            return std::holds_alternative<std::uint64_t>(literal.payload);
        case BuiltinType::I32:
            return std::holds_alternative<std::int32_t>(literal.payload);
        case BuiltinType::I64:
            return std::holds_alternative<std::int64_t>(literal.payload);
        case BuiltinType::F32:
            return std::holds_alternative<float>(literal.payload) &&
                std::isfinite(std::get<float>(literal.payload));
        case BuiltinType::F64:
            return std::holds_alternative<double>(literal.payload) &&
                std::isfinite(std::get<double>(literal.payload));
        }
    }

    const TypeSchemaDefinition* schema =
        literal.type.is_named()
        ? schemas.Resolve(*literal.type.named)
        : nullptr;
    if (!schema)
        return false;
    switch (schema->kind)
    {
    case TypeSchemaKind::BoundedUtf8String:
        return std::holds_alternative<std::string>(literal.payload) &&
            std::get<std::string>(literal.payload).size() <=
                schema->maximum_size;
    case TypeSchemaKind::BoundedBytes:
        return std::holds_alternative<std::vector<Byte>>(
                   literal.payload) &&
            std::get<std::vector<Byte>>(literal.payload).size() <=
                schema->maximum_size;
    case TypeSchemaKind::ClosedEnum:
        if (!std::holds_alternative<EnumValue>(literal.payload))
            return false;
        {
            const EnumValue& value =
                std::get<EnumValue>(literal.payload);
            return value.schema == schema->identity &&
                std::ranges::any_of(
                    schema->enum_members,
                    [&](const EnumMemberDefinition& member) {
                        return member.value == value.value;
                    });
        }
    default:
        return false;
    }
}

bool CheckOperandCount(
    const Instruction& instruction,
    std::size_t expected,
    DiagnosticList& diagnostics)
{
    if (instruction.operands.size() == expected)
        return true;
    Add(
        diagnostics,
        VerificationErrorCode::InvalidInstruction,
        "Instruction has incorrect operand count",
        instruction.source_location);
    return false;
}

const ProgramFunction* FindFunction(
    const ProgramModule& module,
    ProgramFunctionId id)
{
    const auto found = std::ranges::find_if(
        module.functions,
        [id](const ProgramFunction& function) {
            return function.id == id;
        });
    return found == module.functions.end() ? nullptr : &*found;
}

const ProgramFunction* FindImportedFunction(
    const ProgramDefinitionStore& store,
    const ExactDependencyIdentity& dependency,
    std::string_view member_name)
{
    // Imported-function identities use the exact imported module id/version
    // and module content hash in the dependency signature field.
    // Resolve is intentionally exact; no "latest" selection is permitted.
    ModuleIdentity identity{
        dependency.canonical_id,
        dependency.version,
        dependency.signature_hash,
    };
    std::shared_ptr<const ProgramModule> module = store.Resolve(identity);
    if (!module)
        return nullptr;
    const auto found = std::ranges::find_if(
        module->functions,
        [member_name](const ProgramFunction& function) {
            return function.exported && function.name == member_name;
        });
    return found == module->functions.end() ? nullptr : &*found;
}

bool IsImportedModule(
    const ProgramModule& module,
    const ExactDependencyIdentity& dependency)
{
    return std::ranges::any_of(
        module.module_imports,
        [&](const ModuleImportIdentity& imported) {
            return imported.module.canonical_id ==
                    dependency.canonical_id &&
                imported.module.revision == dependency.version &&
                imported.module.module_hash ==
                    dependency.signature_hash;
        });
}

void ValidateInstruction(
    FunctionContext& context,
    const BasicBlock& block,
    const Instruction& instruction,
    std::size_t index)
{
    auto result_type = [&]() -> const TypeRef* {
        return instruction.result ? &instruction.result->type : nullptr;
    };
    auto operand = [&](std::size_t ordinal) -> const TypeRef* {
        if (ordinal >= instruction.operands.size())
            return nullptr;
        return Use(
            context,
            instruction.operands[ordinal],
            block.id,
            index,
            instruction.source_location);
    };
    auto require_result = [&]() -> bool {
        if (instruction.result)
            return true;
        Add(
            context.diagnostics,
            VerificationErrorCode::InvalidInstruction,
            "Value-producing instruction has no result",
            instruction.source_location);
        return false;
    };

    if (instruction.result &&
        !context.schemas.Resolve(instruction.result->type))
    {
        Add(
            context.diagnostics,
            VerificationErrorCode::InvalidSchema,
            "Instruction result type cannot be resolved",
            instruction.source_location);
    }

    switch (instruction.opcode)
    {
    case InstructionOpcode::Constant:
        if (!CheckOperandCount(instruction, 0, context.diagnostics) ||
            !require_result() || !instruction.literal ||
            instruction.literal->type != *result_type() ||
            !ValidateLiteral(*instruction.literal, context.schemas))
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::InvalidInstruction,
                "Constant requires a matching literal and result",
                instruction.source_location);
        }
        break;
    case InstructionOpcode::Copy:
        if (CheckOperandCount(instruction, 1, context.diagnostics) &&
            require_result())
            Same(
                operand(0),
                *result_type(),
                context.diagnostics,
                instruction.source_location,
                "Copy operand");
        break;
    case InstructionOpcode::Select:
        if (CheckOperandCount(instruction, 3, context.diagnostics) &&
            require_result())
        {
            Same(
                operand(0),
                TypeRef::Builtin(BuiltinType::Bool),
                context.diagnostics,
                instruction.source_location,
                "Select condition");
            Same(
                operand(1),
                *result_type(),
                context.diagnostics,
                instruction.source_location,
                "Select true value");
            Same(
                operand(2),
                *result_type(),
                context.diagnostics,
                instruction.source_location,
                "Select false value");
        }
        break;
    case InstructionOpcode::CheckedConvert:
        if (CheckOperandCount(instruction, 1, context.diagnostics) &&
            require_result() &&
            (!IsNumeric(*result_type()) ||
             !operand(0) ||
             !IsNumeric(*operand(0))))
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::TypeMismatch,
                "Checked conversion requires numeric input and output",
                instruction.source_location);
        }
        break;
    case InstructionOpcode::RecordConstruct:
    {
        if (!require_result())
            break;
        const TypeSchemaDefinition* record = NamedSchema(
            context.schemas,
            result_type(),
            TypeSchemaKind::Record);
        if (!record ||
            instruction.operands.size() !=
                record->record_fields.size())
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::TypeMismatch,
                "Record construction requires every declared field",
                instruction.source_location);
            break;
        }
        for (std::size_t i = 0;
             i < record->record_fields.size();
             ++i)
        {
            Same(
                operand(i),
                record->record_fields[i].type,
                context.diagnostics,
                instruction.source_location,
                "Record field");
        }
        break;
    }
    case InstructionOpcode::RecordProject:
    {
        if (!CheckOperandCount(
                instruction,
                1,
                context.diagnostics) ||
            !require_result())
            break;
        const TypeSchemaDefinition* record = NamedSchema(
            context.schemas,
            operand(0),
            TypeSchemaKind::Record);
        if (!record)
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::TypeMismatch,
                "Record projection field or result type is invalid",
                instruction.source_location);
            break;
        }
        const auto field = std::ranges::find(
            record->record_fields,
            instruction.selector,
            &RecordFieldDefinition::name);
        if (field == record->record_fields.end() ||
            field->type != *result_type())
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::TypeMismatch,
                "Record projection field or result type is invalid",
                instruction.source_location);
        }
        break;
    }
    case InstructionOpcode::RecordUpdate:
    {
        if (!CheckOperandCount(
                instruction,
                2,
                context.diagnostics) ||
            !require_result())
            break;
        const TypeRef* source = operand(0);
        const TypeSchemaDefinition* record = NamedSchema(
            context.schemas,
            source,
            TypeSchemaKind::Record);
        if (!record || !source || *source != *result_type())
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::TypeMismatch,
                "Record update source/result type is invalid",
                instruction.source_location);
            break;
        }
        const auto field = std::ranges::find(
            record->record_fields,
            instruction.selector,
            &RecordFieldDefinition::name);
        if (field == record->record_fields.end())
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::InvalidInstruction,
                "Record update names an unknown field",
                instruction.source_location);
        }
        else
        {
            Same(
                operand(1),
                field->type,
                context.diagnostics,
                instruction.source_location,
                "Record update value");
        }
        break;
    }
    case InstructionOpcode::OptionalConstruct:
    {
        if (!require_result())
            break;
        const TypeSchemaDefinition* optional = NamedSchema(
            context.schemas,
            result_type(),
            TypeSchemaKind::Optional);
        if (!optional ||
            instruction.operands.size() > 1)
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::TypeMismatch,
                "Optional construction requires optional result and zero or one value",
                instruction.source_location);
        }
        else if (!instruction.operands.empty())
        {
            Same(
                operand(0),
                *optional->element_type,
                context.diagnostics,
                instruction.source_location,
                "Optional value");
        }
        break;
    }
    case InstructionOpcode::OptionalIsPresent:
        if (CheckOperandCount(
                instruction,
                1,
                context.diagnostics) &&
            require_result())
        {
            if (!NamedSchema(
                    context.schemas,
                    operand(0),
                    TypeSchemaKind::Optional) ||
                !IsBool(*result_type()))
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::TypeMismatch,
                    "Optional presence check requires optional input and bool result",
                    instruction.source_location);
            }
        }
        break;
    case InstructionOpcode::OptionalExtract:
        if (CheckOperandCount(
                instruction,
                1,
                context.diagnostics) &&
            require_result())
        {
            const TypeSchemaDefinition* optional = NamedSchema(
                context.schemas,
                operand(0),
                TypeSchemaKind::Optional);
            if (!optional ||
                *optional->element_type != *result_type())
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::TypeMismatch,
                    "Optional extraction result type is invalid",
                    instruction.source_location);
            }
        }
        break;
    case InstructionOpcode::ListConstruct:
    {
        if (!require_result())
            break;
        const TypeSchemaDefinition* list = NamedSchema(
            context.schemas,
            result_type(),
            TypeSchemaKind::BoundedList);
        if (!list ||
            instruction.operands.size() > list->maximum_size)
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::TypeMismatch,
                "List construction requires a bounded list result",
                instruction.source_location);
            break;
        }
        for (std::size_t i = 0;
             i < instruction.operands.size();
             ++i)
        {
            Same(
                operand(i),
                *list->element_type,
                context.diagnostics,
                instruction.source_location,
                "List element");
        }
        break;
    }
    case InstructionOpcode::ListAppend:
    {
        if (!CheckOperandCount(
                instruction,
                2,
                context.diagnostics) ||
            !require_result())
            break;
        const TypeRef* source = operand(0);
        const TypeSchemaDefinition* list = NamedSchema(
            context.schemas,
            source,
            TypeSchemaKind::BoundedList);
        if (!list || !source || *source != *result_type())
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::TypeMismatch,
                "List append source/result type is invalid",
                instruction.source_location);
        }
        else
        {
            Same(
                operand(1),
                *list->element_type,
                context.diagnostics,
                instruction.source_location,
                "List append value");
        }
        break;
    }
    case InstructionOpcode::ListIndex:
    {
        if (!CheckOperandCount(
                instruction,
                2,
                context.diagnostics) ||
            !require_result())
            break;
        const TypeSchemaDefinition* list = NamedSchema(
            context.schemas,
            operand(0),
            TypeSchemaKind::BoundedList);
        const TypeRef* index_type = operand(1);
        if (!list ||
            !index_type ||
            index_type->is_named() ||
            !IsUnsignedInteger(index_type->builtin) ||
            *list->element_type != *result_type())
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::TypeMismatch,
                "List index types are invalid",
                instruction.source_location);
        }
        break;
    }
    case InstructionOpcode::ListSize:
        if (CheckOperandCount(
                instruction,
                1,
                context.diagnostics) &&
            require_result() &&
            (!NamedSchema(
                 context.schemas,
                 operand(0),
                 TypeSchemaKind::BoundedList) ||
             *result_type() !=
                 TypeRef::Builtin(BuiltinType::U64)))
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::TypeMismatch,
                "List size requires list input and u64 result",
                instruction.source_location);
        }
        break;
    case InstructionOpcode::AddChecked:
    case InstructionOpcode::SubtractChecked:
    case InstructionOpcode::MultiplyChecked:
    case InstructionOpcode::DivideChecked:
    case InstructionOpcode::RemainderChecked:
        if (CheckOperandCount(instruction, 2, context.diagnostics) &&
            require_result())
        {
            if (!IsNumeric(*result_type()))
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::TypeMismatch,
                    "Arithmetic result must be numeric",
                    instruction.source_location);
            }
            Same(
                operand(0),
                *result_type(),
                context.diagnostics,
                instruction.source_location,
                "Arithmetic left operand");
            Same(
                operand(1),
                *result_type(),
                context.diagnostics,
                instruction.source_location,
                "Arithmetic right operand");
        }
        break;
    case InstructionOpcode::NegateChecked:
        if (CheckOperandCount(instruction, 1, context.diagnostics) &&
            require_result())
        {
            if (!IsNumeric(*result_type()))
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::TypeMismatch,
                    "Negation result must be numeric",
                    instruction.source_location);
            }
            Same(
                operand(0),
                *result_type(),
                context.diagnostics,
                instruction.source_location,
                "Negation operand");
        }
        break;
    case InstructionOpcode::Equal:
    case InstructionOpcode::NotEqual:
        if (CheckOperandCount(instruction, 2, context.diagnostics) &&
            require_result())
        {
            if (!IsBool(*result_type()))
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::TypeMismatch,
                    "Comparison result must be bool",
                    instruction.source_location);
            }
            const TypeRef* lhs = operand(0);
            const TypeRef* rhs = operand(1);
            if (lhs && rhs && *lhs != *rhs)
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::TypeMismatch,
                    "Comparison operands must have the same type",
                    instruction.source_location);
            }
        }
        break;
    case InstructionOpcode::Less:
    case InstructionOpcode::LessEqual:
    case InstructionOpcode::Greater:
    case InstructionOpcode::GreaterEqual:
        if (CheckOperandCount(instruction, 2, context.diagnostics) &&
            require_result())
        {
            if (!IsBool(*result_type()))
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::TypeMismatch,
                    "Ordered comparison result must be bool",
                    instruction.source_location);
            }
            const TypeRef* lhs = operand(0);
            const TypeRef* rhs = operand(1);
            if (!lhs || !rhs || *lhs != *rhs || !IsNumeric(*lhs))
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::TypeMismatch,
                    "Ordered comparison operands must have the same numeric type",
                    instruction.source_location);
            }
        }
        break;
    case InstructionOpcode::BooleanAnd:
    case InstructionOpcode::BooleanOr:
        if (CheckOperandCount(instruction, 2, context.diagnostics) &&
            require_result())
        {
            const TypeRef boolean =
                TypeRef::Builtin(BuiltinType::Bool);
            Same(
                operand(0),
                boolean,
                context.diagnostics,
                instruction.source_location,
                "Boolean left operand");
            Same(
                operand(1),
                boolean,
                context.diagnostics,
                instruction.source_location,
                "Boolean right operand");
            if (*result_type() != boolean)
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::TypeMismatch,
                    "Boolean result must be bool",
                    instruction.source_location);
            }
        }
        break;
    case InstructionOpcode::BooleanNot:
        if (CheckOperandCount(instruction, 1, context.diagnostics) &&
            require_result())
        {
            const TypeRef boolean =
                TypeRef::Builtin(BuiltinType::Bool);
            Same(
                operand(0),
                boolean,
                context.diagnostics,
                instruction.source_location,
                "Boolean operand");
            if (*result_type() != boolean)
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::TypeMismatch,
                    "Boolean result must be bool",
                    instruction.source_location);
            }
        }
        break;
    case InstructionOpcode::CallLocal:
    {
        const ProgramFunction* target =
            instruction.target.kind ==
                    InstructionTargetKind::LocalFunction
                ? FindFunction(
                      context.module,
                      instruction.target.local_function)
                : nullptr;
        if (!target ||
            target->arguments.size() != instruction.operands.size())
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::InvalidInstruction,
                "Local call target or arity is invalid",
                instruction.source_location);
            break;
        }
        for (std::size_t i = 0; i < target->arguments.size(); ++i)
        {
            Same(
                operand(i),
                target->arguments[i].type,
                context.diagnostics,
                instruction.source_location,
                "Local call argument");
        }
        if (IsUnit(target->output_type))
        {
            if (instruction.result)
                Add(
                    context.diagnostics,
                    VerificationErrorCode::TypeMismatch,
                    "Unit-returning call cannot produce a value",
                    instruction.source_location);
        }
        else if (!require_result() ||
                 *result_type() != target->output_type)
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::TypeMismatch,
                "Local call result type is invalid",
                instruction.source_location);
        }
        break;
    }
    case InstructionOpcode::CallImported:
    {
        const ExactDependencyIdentity* dependency =
            instruction.target.dependency
            ? &*instruction.target.dependency
            : nullptr;
        const ProgramFunction* target =
            dependency &&
                instruction.target.kind ==
                    InstructionTargetKind::ImportedFunction &&
                IsImportedModule(context.module, *dependency)
            ? FindImportedFunction(
                  context.modules,
                  *dependency,
                  instruction.target.member_name)
            : nullptr;
        if (!target ||
            target->arguments.size() != instruction.operands.size())
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::DependencyMismatch,
                "Imported call target or arity is invalid",
                instruction.source_location);
            break;
        }
        for (std::size_t i = 0; i < target->arguments.size(); ++i)
        {
            Same(
                operand(i),
                target->arguments[i].type,
                context.diagnostics,
                instruction.source_location,
                "Imported call argument");
        }
        if (IsUnit(target->output_type))
        {
            if (instruction.result)
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::TypeMismatch,
                    "Unit-returning imported call cannot produce a value",
                    instruction.source_location);
            }
        }
        else if (!require_result() ||
                 *result_type() != target->output_type)
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::TypeMismatch,
                "Imported call result type is invalid",
                instruction.source_location);
        }
        break;
    }
    case InstructionOpcode::CallReducer:
    {
        const ReducerDescriptor* target =
            instruction.target.kind ==
                    InstructionTargetKind::Reducer &&
                instruction.target.dependency &&
                Contains(
                    context.module.reducer_imports,
                    *instruction.target.dependency)
            ? context.actions.ResolveReducer(
                  *instruction.target.dependency)
            : nullptr;
        if (!target ||
            target->input_types.size() != instruction.operands.size())
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::DependencyMismatch,
                "Reducer target or arity is invalid",
                instruction.source_location);
            break;
        }
        for (std::size_t i = 0; i < target->input_types.size(); ++i)
        {
            Same(
                operand(i),
                target->input_types[i],
                context.diagnostics,
                instruction.source_location,
                "Reducer argument");
        }
        if (!require_result() ||
            *result_type() != target->output_type)
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::TypeMismatch,
                "Reducer result type is invalid",
                instruction.source_location);
        }
        break;
    }
    case InstructionOpcode::AwaitAction:
    {
        const ActionDescriptor* target =
            instruction.target.kind ==
                    InstructionTargetKind::Action &&
                instruction.target.dependency &&
                Contains(
                    context.module.action_imports,
                    *instruction.target.dependency)
            ? context.actions.ResolveAction(
                  *instruction.target.dependency)
            : nullptr;
        if (!target || instruction.operands.size() != 1)
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::DependencyMismatch,
                "Await action requires one exact imported action",
                instruction.source_location);
            break;
        }
        Same(
            operand(0),
            target->input_type,
            context.diagnostics,
            instruction.source_location,
            "Action input");
        if (IsUnit(target->output_type))
        {
            if (instruction.result)
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::TypeMismatch,
                    "Unit-returning action cannot produce a value",
                    instruction.source_location);
            }
        }
        else if (!require_result() ||
                 *result_type() != target->output_type)
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::TypeMismatch,
                "Action result type is invalid",
                instruction.source_location);
        }
        break;
    }
    case InstructionOpcode::EmitRecord:
        if (!CheckOperandCount(instruction, 1, context.diagnostics) ||
            instruction.result)
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::InvalidInstruction,
                "Emission instructions consume one value and have no result",
                instruction.source_location);
        }
        if (const TypeRef* emitted = operand(0);
            !emitted || !emitted->is_named() ||
            !std::ranges::any_of(
                context.module.entrypoints,
                [&](const ProgramEntrypoint& entrypoint) {
                    return Contains(
                        entrypoint.emission_schemas,
                        *emitted->named);
                }))
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::UndeclaredEffect,
                "Emitted record schema is not declared by an entrypoint",
                instruction.source_location);
        }
        break;
    case InstructionOpcode::PublishArtifact:
        if (!CheckOperandCount(instruction, 1, context.diagnostics) ||
            instruction.result)
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::InvalidInstruction,
                "Artifact publication consumes one value and has no result",
                instruction.source_location);
        }
        if (const TypeRef* artifact = operand(0);
            !artifact || !artifact->is_named())
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::TypeMismatch,
                "Artifact publication requires a typed artifact reference",
                instruction.source_location);
        }
        else
        {
            const TypeSchemaDefinition* schema =
                context.schemas.Resolve(*artifact->named);
            const std::optional<SchemaIdentity> payload =
                schema &&
                    schema->kind ==
                        TypeSchemaKind::ArtifactReference &&
                    schema->element_type &&
                    schema->element_type->is_named()
                ? schema->element_type->named
                : std::nullopt;
            const bool declared = schema && payload &&
                std::ranges::any_of(
                    context.module.entrypoints,
                    [&](const ProgramEntrypoint& entrypoint) {
                        return Contains(
                            entrypoint.artifact_schemas,
                            *payload);
                    });
            if (!declared)
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::UndeclaredEffect,
                    "Artifact schema is not declared by an entrypoint",
                    instruction.source_location);
            }
        }
        break;
    case InstructionOpcode::EnterScope:
        if (instruction.scope.value() == 0 ||
            !instruction.operands.empty() ||
            instruction.result)
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::InvalidScope,
                "EnterScope requires one nonzero scope identity",
                instruction.source_location);
        }
        break;
    case InstructionOpcode::ExitScope:
        if (instruction.scope.value() == 0 ||
            !instruction.operands.empty() ||
            instruction.result)
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::InvalidScope,
                "ExitScope requires one nonzero scope identity",
                instruction.source_location);
        }
        break;
    case InstructionOpcode::DeferCompensation:
    {
        const ActionDescriptor* target =
            instruction.target.kind ==
                    InstructionTargetKind::DeferredAction &&
                instruction.target.dependency &&
                Contains(
                    context.module.action_imports,
                    *instruction.target.dependency)
            ? context.actions.ResolveAction(
                  *instruction.target.dependency)
            : nullptr;
        if (!target)
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::DependencyMismatch,
                "Deferred compensation requires one exact imported action",
                instruction.source_location);
        }
        if (instruction.scope.value() == 0 ||
            instruction.operands.size() != 1 ||
            instruction.result)
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::InvalidScope,
                "Deferred compensation requires one input, no result, and an active scope",
                instruction.source_location);
        }
        if (target)
        {
            if (target->cleanup !=
                    ActionCleanupGuarantee::VerifiedCompensation ||
                target->idempotency ==
                    ActionIdempotency::NotRetryable)
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::InvalidScope,
                    "Deferred action must be cleanup-safe and idempotent",
                    instruction.source_location);
            }
            if (instruction.operands.size() == 1)
            {
                Same(
                    operand(0),
                    target->input_type,
                    context.diagnostics,
                    instruction.source_location,
                    "Deferred action input");
            }
        }
        for (std::size_t ordinal = target ? 1u : 0u;
             ordinal < instruction.operands.size();
             ++ordinal)
        {
            (void)operand(ordinal);
        }
        break;
    }
    case InstructionOpcode::PromoteResource:
    {
        if (!context.module.accepted_policies
                 .permits_resource_promotion ||
            instruction.scope.value() == 0 ||
            instruction.result ||
            !CheckOperandCount(
                instruction,
                1,
                context.diagnostics))
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::InvalidScope,
                "Resource promotion must be declared, target an enclosing scope, consume one handle, and have no result",
                instruction.source_location);
        }
        const TypeRef* promoted = operand(0);
        if (promoted &&
            !NamedSchema(
                context.schemas,
                promoted,
                TypeSchemaKind::ResourceHandle))
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::TypeMismatch,
                "Resource promotion requires a typed resource handle",
                instruction.source_location);
        }
        break;
    }
    }
}

void BuildDominators(FunctionContext& context)
{
    std::map<ProgramBlockId, std::set<ProgramBlockId>> predecessors;
    std::vector<ProgramBlockId> work{context.function.entry_block};
    while (!work.empty())
    {
        const ProgramBlockId current = work.back();
        work.pop_back();
        if (!context.reachable.emplace(current).second)
            continue;
        const auto block = context.blocks.find(current);
        if (block == context.blocks.end())
            continue;
        for (const BlockEdge& edge : Edges(block->second->terminator))
        {
            predecessors[edge.target].insert(current);
            work.push_back(edge.target);
        }
    }

    for (ProgramBlockId block : context.reachable)
    {
        if (block == context.function.entry_block)
        {
            context.dominators[block] = {block};
        }
        else
        {
            context.dominators[block] = context.reachable;
        }
    }

    bool changed = true;
    while (changed)
    {
        changed = false;
        for (ProgramBlockId block : context.reachable)
        {
            if (block == context.function.entry_block)
                continue;
            std::set<ProgramBlockId> next;
            bool first = true;
            for (ProgramBlockId predecessor : predecessors[block])
            {
                if (first)
                {
                    next = context.dominators[predecessor];
                    first = false;
                }
                else
                {
                    std::set<ProgramBlockId> intersection;
                    std::ranges::set_intersection(
                        next,
                        context.dominators[predecessor],
                        std::inserter(
                            intersection,
                            intersection.end()));
                    next = std::move(intersection);
                }
            }
            next.insert(block);
            if (next != context.dominators[block])
            {
                context.dominators[block] = std::move(next);
                changed = true;
            }
        }
    }
}

void ValidateReachableExit(FunctionContext& context)
{
    std::map<ProgramBlockId, std::set<ProgramBlockId>> predecessors;
    std::vector<ProgramBlockId> work;
    std::set<ProgramBlockId> can_reach_exit;
    for (ProgramBlockId id : context.reachable)
    {
        const auto found = context.blocks.find(id);
        if (found == context.blocks.end())
            continue;
        const Terminator& terminator = found->second->terminator;
        if (terminator.kind == TerminatorKind::Return ||
            terminator.kind == TerminatorKind::StructuredFail)
        {
            can_reach_exit.insert(id);
            work.push_back(id);
        }
        for (const BlockEdge& edge : Edges(terminator))
        {
            if (context.reachable.contains(edge.target) &&
                context.blocks.contains(edge.target))
            {
                predecessors[edge.target].insert(id);
            }
        }
    }
    while (!work.empty())
    {
        const ProgramBlockId current = work.back();
        work.pop_back();
        for (ProgramBlockId predecessor : predecessors[current])
        {
            if (can_reach_exit.emplace(predecessor).second)
                work.push_back(predecessor);
        }
    }
    const auto trapped = std::ranges::find_if(
        context.reachable,
        [&](ProgramBlockId id) {
            return context.blocks.contains(id) &&
                !can_reach_exit.contains(id);
        });
    if (trapped != context.reachable.end())
    {
        Add(
            context.diagnostics,
            VerificationErrorCode::InvalidControlFlow,
            "Reachable control flow contains a statically unconditional "
            "no-exit loop",
            context.blocks.at(*trapped)->terminator.source_location);
    }
}

void ValidateScopeFlow(FunctionContext& context)
{
    using ScopeStack = std::vector<ProgramScopeId>;
    std::set<ProgramScopeId> declared;
    for (const BasicBlock& block : context.function.blocks)
    {
        for (const Instruction& instruction : block.instructions)
        {
            if (instruction.opcode == InstructionOpcode::EnterScope &&
                (!instruction.scope ||
                 !declared.emplace(instruction.scope).second))
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::InvalidScope,
                    "Scope identities must be nonzero and entered exactly once",
                    instruction.source_location);
            }
        }
    }

    std::map<ProgramBlockId, ScopeStack> incoming;
    std::set<ProgramBlockId> processed;
    std::vector<ProgramBlockId> pending;
    const auto enqueue =
        [&](ProgramBlockId block,
            const ScopeStack& stack,
            auto& self) -> void {
        const auto existing = incoming.find(block);
        if (existing == incoming.end())
        {
            incoming.emplace(block, stack);
            pending.push_back(block);
        }
        else if (existing->second != stack)
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::InvalidScope,
                "Control-flow merge has incompatible lexical scope stacks");
        }
        (void)self;
    };

    incoming.emplace(context.function.entry_block, ScopeStack{});
    pending.push_back(context.function.entry_block);
    for (;;)
    {
        while (!pending.empty())
        {
            const ProgramBlockId id = pending.back();
            pending.pop_back();
            if (!processed.emplace(id).second)
                continue;
            const auto found = context.blocks.find(id);
            if (found == context.blocks.end())
                continue;
            const BasicBlock& block = *found->second;
            ScopeStack stack = incoming[id];
            for (const Instruction& instruction : block.instructions)
            {
                switch (instruction.opcode)
                {
                case InstructionOpcode::EnterScope:
                    if (instruction.scope)
                        stack.push_back(instruction.scope);
                    break;
                case InstructionOpcode::ExitScope:
                    if (stack.empty() ||
                        stack.back() != instruction.scope)
                    {
                        Add(
                            context.diagnostics,
                            VerificationErrorCode::InvalidScope,
                            "ExitScope must close the active innermost scope",
                            instruction.source_location);
                    }
                    else
                    {
                        stack.pop_back();
                    }
                    break;
                case InstructionOpcode::DeferCompensation:
                    if (stack.empty() ||
                        stack.back() != instruction.scope)
                    {
                        Add(
                            context.diagnostics,
                            VerificationErrorCode::InvalidScope,
                            "Deferred compensation must target the active innermost scope",
                            instruction.source_location);
                    }
                    break;
                case InstructionOpcode::PromoteResource:
                {
                    const auto destination = std::ranges::find(
                        stack,
                        instruction.scope);
                    if (destination == stack.end() ||
                        (!stack.empty() &&
                         stack.back() == instruction.scope))
                    {
                        Add(
                            context.diagnostics,
                            VerificationErrorCode::InvalidScope,
                            "Resource promotion must target an active enclosing scope",
                            instruction.source_location);
                    }
                    break;
                }
                case InstructionOpcode::AwaitAction:
                    if (instruction.target.dependency)
                    {
                        const ActionDescriptor* descriptor =
                            context.actions.ResolveAction(
                                *instruction.target.dependency);
                        const bool resource =
                            descriptor &&
                            descriptor->resource_behavior !=
                                ActionResourceBehavior::None;
                        const bool active =
                            instruction.scope &&
                            std::ranges::find(
                                stack,
                                instruction.scope) != stack.end();
                        if ((resource && !active) ||
                            (!resource && instruction.scope))
                        {
                            Add(
                                context.diagnostics,
                                VerificationErrorCode::InvalidScope,
                                "Await action resource scope does not match its descriptor",
                                instruction.source_location);
                        }
                    }
                    break;
                default:
                    break;
                }
            }

            if ((block.terminator.kind == TerminatorKind::Return ||
                 block.terminator.kind ==
                     TerminatorKind::StructuredFail) &&
                !stack.empty())
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::InvalidScope,
                    "Terminal block leaves a lexical scope open",
                    block.terminator.source_location);
            }
            for (const BlockEdge& edge : Edges(block.terminator))
                enqueue(edge.target, stack, enqueue);
        }

        const auto unprocessed = std::ranges::find_if(
            context.function.blocks,
            [&](const BasicBlock& block) {
                return !processed.contains(block.id);
            });
        if (unprocessed == context.function.blocks.end())
            break;
        incoming.emplace(unprocessed->id, ScopeStack{});
        pending.push_back(unprocessed->id);
    }
}

void ValidateTerminator(
    FunctionContext& context,
    const BasicBlock& block)
{
    const Terminator& terminator = block.terminator;
    const std::size_t use_index =
        block.instructions.size() + 1;
    auto use = [&](ProgramValueId value) {
        return Use(
            context,
            value,
            block.id,
            use_index,
            terminator.source_location);
    };
    auto validate_edge = [&](const BlockEdge& edge) {
        const auto target = context.blocks.find(edge.target);
        if (target == context.blocks.end())
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::InvalidControlFlow,
                "Terminator targets an unknown block",
                terminator.source_location);
            return;
        }
        if (edge.arguments.size() !=
            target->second->arguments.size())
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::InvalidControlFlow,
                "Block edge argument arity mismatch",
                terminator.source_location);
            return;
        }
        for (std::size_t i = 0; i < edge.arguments.size(); ++i)
        {
            Same(
                use(edge.arguments[i]),
                target->second->arguments[i].type,
                context.diagnostics,
                terminator.source_location,
                "Block edge argument");
        }
    };

    switch (terminator.kind)
    {
    case TerminatorKind::Branch:
        if (terminator.edges.size() != 1 ||
            terminator.condition_or_selector ||
            !terminator.enum_cases.empty() ||
            terminator.default_edge)
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::InvalidTerminator,
                "Branch terminator requires exactly one edge",
                terminator.source_location);
        }
        break;
    case TerminatorKind::ConditionalBranch:
        if (terminator.edges.size() != 2 ||
            !terminator.condition_or_selector)
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::InvalidTerminator,
                "Conditional branch requires bool selector and two edges",
                terminator.source_location);
        }
        else
        {
            Same(
                use(*terminator.condition_or_selector),
                TypeRef::Builtin(BuiltinType::Bool),
                context.diagnostics,
                terminator.source_location,
                "Conditional branch selector");
        }
        break;
    case TerminatorKind::EnumSwitch:
        if (!terminator.condition_or_selector ||
            terminator.enum_cases.empty())
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::InvalidTerminator,
                "Enum switch requires selector and cases",
                terminator.source_location);
        }
        else
        {
            const TypeRef* type =
                use(*terminator.condition_or_selector);
            const TypeSchemaDefinition* schema =
                type && type->is_named()
                ? context.schemas.Resolve(*type->named)
                : nullptr;
            if (!schema ||
                schema->kind != TypeSchemaKind::ClosedEnum)
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::TypeMismatch,
                    "Enum switch selector must be a closed enum",
                    terminator.source_location);
            }
            std::set<std::int64_t> cases;
            for (const EnumSwitchCase& item :
                 terminator.enum_cases)
            {
                if (!cases.emplace(item.enum_value).second)
                {
                    Add(
                        context.diagnostics,
                        VerificationErrorCode::InvalidTerminator,
                        "Enum switch cases must be unique",
                        terminator.source_location);
                }
            }
        }
        break;
    case TerminatorKind::Return:
        if (IsUnit(context.function.output_type))
        {
            if (terminator.return_value)
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::TypeMismatch,
                    "Unit function cannot return a value",
                    terminator.source_location);
            }
        }
        else if (!terminator.return_value ||
                 !Same(
                     use(*terminator.return_value),
                     context.function.output_type,
                     context.diagnostics,
                     terminator.source_location,
                     "Return value"))
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::InvalidTerminator,
                "Return terminator requires declared output",
                terminator.source_location);
        }
        if (context.function.domain_outcome_type)
        {
            if (!terminator.domain_outcome)
            {
                Add(
                    context.diagnostics,
                    VerificationErrorCode::InvalidTerminator,
                    "Return requires declared domain outcome",
                    terminator.source_location);
            }
            else
            {
                Same(
                    use(*terminator.domain_outcome),
                    *context.function.domain_outcome_type,
                    context.diagnostics,
                    terminator.source_location,
                    "Domain outcome");
            }
        }
        break;
    case TerminatorKind::StructuredFail:
        if (!terminator.failure ||
            terminator.failure->code.empty())
        {
            Add(
                context.diagnostics,
                VerificationErrorCode::InvalidTerminator,
                "Structured failure requires a stable code",
                terminator.source_location);
        }
        if (terminator.failure &&
            terminator.failure->details)
            (void)use(*terminator.failure->details);
        break;
    }

    for (const BlockEdge& edge : Edges(terminator))
        validate_edge(edge);
}

void ValidateFunction(
    const ProgramModule& module,
    const ProgramFunction& function,
    const SchemaLookup& schemas,
    const ActionRegistry& actions,
    const ProgramDefinitionStore& modules,
    DiagnosticList& diagnostics)
{
    FunctionContext context{
        module,
        function,
        schemas,
        actions,
        modules,
        diagnostics,
    };
    if (!function.id || function.name.empty() ||
        !function.entry_block || function.blocks.empty())
    {
        Add(
            diagnostics,
            VerificationErrorCode::InvalidFunction,
            "Function requires identity, name, entry block, and blocks");
        return;
    }
    if (!schemas.Resolve(function.output_type) ||
        (function.domain_outcome_type &&
         !schemas.Resolve(*function.domain_outcome_type)))
    {
        Add(
            diagnostics,
            VerificationErrorCode::InvalidSchema,
            "Function output schema cannot be resolved");
    }

    std::set<ProgramInstructionId> instruction_ids;
    for (const BasicBlock& block : function.blocks)
    {
        if (!block.id ||
            !context.blocks.emplace(block.id, &block).second)
        {
            Add(
                diagnostics,
                VerificationErrorCode::DuplicateIdentity,
                "Function block identity is zero or duplicated");
        }
    }
    if (!context.blocks.contains(function.entry_block))
    {
        Add(
            diagnostics,
            VerificationErrorCode::InvalidControlFlow,
            "Function entry block does not exist");
        return;
    }

    auto add_value = [&](const ValueDefinition& value,
                         std::optional<ProgramBlockId> block,
                         std::size_t index,
                         bool block_argument,
                         bool function_argument) {
        if (!value.id || !schemas.Resolve(value.type) ||
            !context.values
                 .emplace(
                     value.id,
                     ValueDefinitionSite{
                         value.type,
                         block,
                         index,
                         block_argument,
                         function_argument,
                     })
                 .second)
        {
            Add(
                diagnostics,
                VerificationErrorCode::DuplicateIdentity,
                "Value identity is zero, duplicated, or has unknown type");
        }
    };

    for (const ValueDefinition& argument : function.arguments)
        add_value(argument, std::nullopt, 0, false, true);
    for (const BasicBlock& block : function.blocks)
    {
        for (const ValueDefinition& argument : block.arguments)
            add_value(argument, block.id, 0, true, false);
        for (std::size_t i = 0; i < block.instructions.size(); ++i)
        {
            const Instruction& instruction = block.instructions[i];
            if (!instruction.id ||
                !instruction_ids.emplace(instruction.id).second)
            {
                Add(
                    diagnostics,
                    VerificationErrorCode::DuplicateIdentity,
                    "Instruction identity is zero or duplicated",
                    instruction.source_location);
            }
            if (instruction.result)
                add_value(
                    *instruction.result,
                    block.id,
                    i,
                    false,
                    false);
        }
    }

    BuildDominators(context);
    ValidateReachableExit(context);
    ValidateScopeFlow(context);
    for (const BasicBlock& block : function.blocks)
    {
        for (std::size_t i = 0; i < block.instructions.size(); ++i)
            ValidateInstruction(context, block, block.instructions[i], i);
        ValidateTerminator(context, block);
    }
}

const TypeRef* FindValueType(
    const ProgramFunction& function,
    ProgramValueId id)
{
    const auto argument = std::ranges::find(
        function.arguments,
        id,
        &ValueDefinition::id);
    if (argument != function.arguments.end())
        return &argument->type;
    for (const BasicBlock& block : function.blocks)
    {
        const auto block_argument = std::ranges::find(
            block.arguments,
            id,
            &ValueDefinition::id);
        if (block_argument != block.arguments.end())
            return &block_argument->type;
        for (const Instruction& instruction : block.instructions)
        {
            if (instruction.result && instruction.result->id == id)
                return &instruction.result->type;
        }
    }
    return nullptr;
}

std::set<ProgramBlockId> ReachableBlocks(
    const ProgramFunction& function)
{
    std::map<ProgramBlockId, const BasicBlock*> blocks;
    for (const BasicBlock& block : function.blocks)
        blocks.emplace(block.id, &block);
    std::set<ProgramBlockId> reachable;
    std::vector<ProgramBlockId> work{function.entry_block};
    while (!work.empty())
    {
        const ProgramBlockId current = work.back();
        work.pop_back();
        if (!reachable.emplace(current).second)
            continue;
        const auto found = blocks.find(current);
        if (found == blocks.end())
            continue;
        for (const BlockEdge& edge : Edges(found->second->terminator))
            work.push_back(edge.target);
    }
    return reachable;
}

bool ActionEffectsAllowed(
    const ActionDescriptor& action,
    const ProgramPolicySet& policies)
{
    const auto has = [&](ActionEffect effect) {
        return (action.effects & EffectMask(effect)) != 0;
    };
    return (!has(ActionEffect::ReplaceState) ||
            policies.permits_state_replacement) &&
        (!has(ActionEffect::MoviePlayback) ||
         policies.permits_movie_playback) &&
        (!has(ActionEffect::MovieRecording) ||
         policies.permits_movie_recording) &&
        (!has(ActionEffect::Capture) ||
         policies.permits_capture);
}

void ValidateEntrypointClosure(
    const ProgramModule& root_module,
    const ProgramEntrypoint& entrypoint,
    const TypeSchemaRegistry& schemas,
    const ActionRegistry& actions,
    const ProgramDefinitionStore& modules,
    DiagnosticList& diagnostics)
{
    struct WorkItem
    {
        const ProgramModule* module = nullptr;
        ProgramFunctionId function;
        std::shared_ptr<const ProgramModule> owner;
    };
    std::vector<WorkItem> work{
        {&root_module, entrypoint.function, {}}};
    std::set<std::pair<ModuleIdentity, ProgramFunctionId>> visited;
    while (!work.empty())
    {
        WorkItem current = std::move(work.back());
        work.pop_back();
        if (!current.module ||
            !visited.emplace(
                 current.module->identity,
                 current.function)
                 .second)
        {
            continue;
        }
        const ProgramFunction* function =
            FindFunction(*current.module, current.function);
        if (!function)
            continue;
        const std::set<ProgramBlockId> reachable =
            ReachableBlocks(*function);
        const SchemaLookup schema_lookup{*current.module, schemas};
        for (const BasicBlock& block : function->blocks)
        {
            if (!reachable.contains(block.id))
                continue;
            for (const Instruction& instruction : block.instructions)
            {
                if (instruction.opcode ==
                        InstructionOpcode::CallLocal &&
                    instruction.target.kind ==
                        InstructionTargetKind::LocalFunction)
                {
                    work.push_back(
                        {current.module,
                         instruction.target.local_function,
                         current.owner});
                }
                else if (
                    instruction.opcode ==
                        InstructionOpcode::CallImported &&
                    instruction.target.kind ==
                        InstructionTargetKind::ImportedFunction &&
                    instruction.target.dependency)
                {
                    const ExactDependencyIdentity& dependency =
                        *instruction.target.dependency;
                    auto imported = modules.Resolve(
                        ModuleIdentity{
                            dependency.canonical_id,
                            dependency.version,
                            dependency.signature_hash,
                        });
                    if (imported)
                    {
                        const auto target = std::ranges::find_if(
                            imported->functions,
                            [&](const ProgramFunction& candidate) {
                                return candidate.exported &&
                                    candidate.name ==
                                        instruction.target.member_name;
                            });
                        if (target != imported->functions.end())
                        {
                            work.push_back(
                                {imported.get(),
                                 target->id,
                                 std::move(imported)});
                        }
                    }
                }

                if ((instruction.opcode ==
                         InstructionOpcode::AwaitAction ||
                     instruction.opcode ==
                         InstructionOpcode::DeferCompensation) &&
                    instruction.target.dependency)
                {
                    const ActionDescriptor* action =
                        actions.ResolveAction(
                            *instruction.target.dependency);
                    if (action &&
                        (!Contains(
                             entrypoint.required_capability_packs,
                             action->providing_pack) ||
                         !ActionEffectsAllowed(
                             *action,
                             entrypoint.accepted_policies)))
                    {
                        Add(
                            diagnostics,
                            VerificationErrorCode::UndeclaredEffect,
                            "Entrypoint '" + entrypoint.name +
                                "' does not declare a reachable action "
                                "capability or effect: " +
                                action->identity.canonical_id,
                            instruction.source_location);
                    }
                }
                else if (
                    instruction.opcode ==
                        InstructionOpcode::EmitRecord &&
                    !instruction.operands.empty())
                {
                    const TypeRef* emitted = FindValueType(
                        *function,
                        instruction.operands.front());
                    if (emitted && emitted->is_named() &&
                        !Contains(
                            entrypoint.emission_schemas,
                            *emitted->named))
                    {
                        Add(
                            diagnostics,
                            VerificationErrorCode::UndeclaredEffect,
                            "Entrypoint '" + entrypoint.name +
                                "' does not declare a reachable emission",
                            instruction.source_location);
                    }
                }
                else if (
                    instruction.opcode ==
                        InstructionOpcode::PublishArtifact &&
                    !instruction.operands.empty())
                {
                    const TypeRef* reference = FindValueType(
                        *function,
                        instruction.operands.front());
                    const TypeSchemaDefinition* schema =
                        reference && reference->is_named()
                        ? schema_lookup.Resolve(*reference->named)
                        : nullptr;
                    const std::optional<SchemaIdentity> payload =
                        schema &&
                            schema->kind ==
                                TypeSchemaKind::ArtifactReference &&
                            schema->element_type &&
                            schema->element_type->is_named()
                        ? schema->element_type->named
                        : std::nullopt;
                    if (payload &&
                        !Contains(
                            entrypoint.artifact_schemas,
                            *payload))
                    {
                        Add(
                            diagnostics,
                            VerificationErrorCode::UndeclaredEffect,
                            "Entrypoint '" + entrypoint.name +
                                "' does not declare a reachable artifact",
                            instruction.source_location);
                    }
                }
            }
        }
    }
}

bool ValidateSourceMap(
    const ProgramModule& module,
    DiagnosticList& diagnostics)
{
    if (module.source_map.version != 1)
    {
        Add(
            diagnostics,
            VerificationErrorCode::InvalidSourceMap,
            "Only source-map version 1 is supported");
        return false;
    }
    std::set<ProgramSourceLocationId> ids;
    std::set<ProgramSourceLocationId> referenced;
    std::set<ProgramFunctionId> function_ids;
    std::set<ProgramBlockId> block_ids;
    std::set<ProgramInstructionId> instruction_ids;
    for (const ProgramFunction& function : module.functions)
    {
        function_ids.insert(function.id);
        for (const BasicBlock& block : function.blocks)
        {
            block_ids.insert(block.id);
            referenced.insert(block.terminator.source_location);
            for (const Instruction& instruction : block.instructions)
            {
                instruction_ids.insert(instruction.id);
                referenced.insert(instruction.source_location);
            }
        }
    }
    for (const SourceMapEntry& entry : module.source_map.entries)
    {
        if (!entry.id || !ids.emplace(entry.id).second ||
            entry.source_name.empty() ||
            (entry.function &&
             !function_ids.contains(*entry.function)) ||
            (entry.block && !block_ids.contains(*entry.block)) ||
            (entry.instruction &&
             !instruction_ids.contains(*entry.instruction)))
        {
            Add(
                diagnostics,
                VerificationErrorCode::InvalidSourceMap,
                "Source-map entry is invalid or duplicated",
                entry.id);
        }
    }
    for (ProgramSourceLocationId id : referenced)
    {
        if (!id || !ids.contains(id))
        {
            Add(
                diagnostics,
                VerificationErrorCode::InvalidSourceMap,
                "Instruction or terminator has no source-map entry",
                id);
        }
    }
    return diagnostics.empty();
}

bool ValidateEffects(
    const ProgramModule& module,
    const ActionRegistry& registry,
    DiagnosticList& diagnostics)
{
    for (const ExactDependencyIdentity& identity :
         module.action_imports)
    {
        const ActionDescriptor* action =
            registry.ResolveAction(identity);
        if (!action)
            continue;
        const auto has = [&](ActionEffect effect) {
            return (action->effects & EffectMask(effect)) != 0;
        };
        if ((has(ActionEffect::ReplaceState) &&
             !module.accepted_policies.permits_state_replacement) ||
            (has(ActionEffect::MoviePlayback) &&
             !module.accepted_policies.permits_movie_playback) ||
            (has(ActionEffect::MovieRecording) &&
             !module.accepted_policies.permits_movie_recording) ||
            (has(ActionEffect::Capture) &&
             !module.accepted_policies.permits_capture))
        {
            Add(
                diagnostics,
                VerificationErrorCode::UndeclaredEffect,
                "Imported action effect is not allowed by module policy: " +
                    identity.canonical_id);
        }
    }
    return diagnostics.empty();
}

template <typename Identity, typename Key>
void ValidateUniqueExactDependencies(
    const std::vector<Identity>& identities,
    Key key,
    std::string_view label,
    DiagnosticList& diagnostics)
{
    using KeyType = decltype(key(std::declval<Identity>()));
    std::map<KeyType, Identity> seen;
    for (const Identity& identity : identities)
    {
        const KeyType current = key(identity);
        const auto found = seen.find(current);
        if (found == seen.end())
        {
            seen.emplace(current, identity);
            continue;
        }
        Add(
            diagnostics,
            found->second == identity
                ? VerificationErrorCode::DuplicateIdentity
                : VerificationErrorCode::DependencyMismatch,
            std::string(label) +
                (found->second == identity
                     ? " is duplicated"
                     : " conflicts at the same id/version"));
    }
}

void ValidateDependencyDeclarations(
    const ProgramModule& module,
    DiagnosticList& diagnostics)
{
    ValidateUniqueExactDependencies(
        module.module_imports,
        [](const ModuleImportIdentity& value) {
            return std::pair{
                value.module.canonical_id,
                value.module.revision,
            };
        },
        "Module import",
        diagnostics);
    ValidateUniqueExactDependencies(
        module.action_imports,
        [](const ExactDependencyIdentity& value) {
            return std::pair{value.canonical_id, value.version};
        },
        "Action import",
        diagnostics);
    ValidateUniqueExactDependencies(
        module.reducer_imports,
        [](const ExactDependencyIdentity& value) {
            return std::pair{value.canonical_id, value.version};
        },
        "Reducer import",
        diagnostics);
    ValidateUniqueExactDependencies(
        module.type_imports,
        [](const SchemaIdentity& value) {
            return std::pair{value.canonical_id, value.version};
        },
        "Type import",
        diagnostics);
    ValidateUniqueExactDependencies(
        module.required_capability_packs,
        [](const CapabilityPackIdentity& value) {
            return std::pair{value.canonical_id, value.version};
        },
        "Capability pack import",
        diagnostics);
}

void ValidateLocalSchemas(
    const ProgramModule& module,
    const SchemaLookup& schemas,
    DiagnosticList& diagnostics)
{
    std::map<SchemaIdentity, const TypeSchemaDefinition*> local;
    for (const TypeSchemaDefinition& definition :
         module.local_types)
    {
        const RegistryResult shape =
            TypeSchemaRegistry::ValidateShape(definition);
        if (!shape.success)
        {
            Add(
                diagnostics,
                VerificationErrorCode::InvalidSchema,
                "Invalid local schema " +
                    definition.identity.canonical_id +
                    ": " + shape.error.message);
        }
        local.emplace(definition.identity, &definition);
    }

    const auto dependencies =
        [](const TypeSchemaDefinition& definition) {
            std::vector<SchemaIdentity> result;
            if (definition.element_type &&
                definition.element_type->is_named())
                result.push_back(*definition.element_type->named);
            for (const RecordFieldDefinition& field :
                 definition.record_fields)
            {
                if (field.type.is_named())
                    result.push_back(*field.type.named);
            }
            return result;
        };

    enum class Visit : std::uint8_t
    {
        Visiting,
        Complete,
    };
    std::map<SchemaIdentity, Visit> visits;
    std::function<void(const TypeSchemaDefinition&)> visit =
        [&](const TypeSchemaDefinition& definition) {
        const auto prior = visits.find(definition.identity);
        if (prior != visits.end())
        {
            if (prior->second == Visit::Visiting)
            {
                Add(
                    diagnostics,
                    VerificationErrorCode::InvalidSchema,
                    "Recursive local schema is forbidden: " +
                        definition.identity.canonical_id);
            }
            return;
        }
        visits.emplace(definition.identity, Visit::Visiting);
        for (const SchemaIdentity& dependency :
             dependencies(definition))
        {
            if (!schemas.Resolve(dependency))
            {
                Add(
                    diagnostics,
                    VerificationErrorCode::InvalidSchema,
                    "Local schema dependency is undeclared or unresolved: " +
                        dependency.canonical_id);
                continue;
            }
            const auto nested = local.find(dependency);
            if (nested != local.end())
                visit(*nested->second);
        }
        visits[definition.identity] = Visit::Complete;
    };
    for (const auto& [identity, definition] : local)
    {
        (void)identity;
        visit(*definition);
    }
}

void AppendUnique(
    std::vector<ModuleImportIdentity>& values,
    const ModuleImportIdentity& value)
{
    if (std::ranges::find(values, value) == values.end())
        values.push_back(value);
}

template <typename T>
void AppendUnique(std::vector<T>& values, const T& value)
{
    if (std::ranges::find(values, value) == values.end())
        values.push_back(value);
}

void SortLock(ProgramDependencyLock& lock)
{
    std::ranges::sort(lock.module_imports);
    std::ranges::sort(lock.action_imports);
    std::ranges::sort(lock.reducer_imports);
    std::ranges::sort(lock.type_imports);
    std::ranges::sort(lock.capability_packs);
}

} // namespace

ProgramVerificationResult ProgramVerifier::Verify(
    const ModuleIdentity& identity,
    const RuntimeCompatibility& compatibility)
{
    ProgramVerificationResult result;
    std::shared_ptr<const ProgramModule> module =
        modules_.Resolve(identity);
    if (!module)
    {
        Add(
            result.diagnostics,
            VerificationErrorCode::ModuleNotFound,
            "Exact module is not registered: " +
                identity.canonical_id);
        return result;
    }

    const CodecStatus canonical =
        ValidateProgramModuleIdentityV1(*module);
    if (!canonical)
    {
        Add(
            result.diagnostics,
            VerificationErrorCode::CanonicalIdentity,
            canonical.message);
        return result;
    }
    if (module->ir_version != kCanonicalIrVersionV1)
    {
        Add(
            result.diagnostics,
            VerificationErrorCode::UnsupportedIrVersion,
            "Only canonical IR version 1 is supported");
        return result;
    }
    ValidateDependencyDeclarations(*module, result.diagnostics);

    RegistryError closure_error;
    std::optional<ModuleDependencyClosure> module_closure =
        modules_.ResolveClosure(identity, &closure_error);
    if (!module_closure)
    {
        Add(
            result.diagnostics,
            VerificationErrorCode::DependencyMissing,
            closure_error.message);
        return result;
    }
    for (const auto& dependency :
         module_closure->dependency_order)
    {
        if (dependency->identity == identity)
            continue;
        const ProgramVerificationResult dependency_result =
            Verify(dependency->identity, compatibility);
        if (!dependency_result.success)
        {
            for (VerificationDiagnostic diagnostic :
                 dependency_result.diagnostics)
            {
                diagnostic.message =
                    "Imported module " +
                    dependency->identity.canonical_id +
                    ": " + diagnostic.message;
                result.diagnostics.push_back(
                    std::move(diagnostic));
            }
        }
    }
    if (!result.diagnostics.empty())
        return result;

    auto verified = std::make_shared<VerifiedProgramModule>();
    verified->module = module;
    verified->module_closure =
        module_closure->dependency_order;
    verified->dependency_lock.ir_version = module->ir_version;

    std::vector<SchemaIdentity> type_roots;
    std::vector<CapabilityPackIdentity> pack_roots;
    for (const auto& closure_module : verified->module_closure)
    {
        if (closure_module->identity != identity)
        {
            AppendUnique(
                verified->dependency_lock.module_imports,
                ModuleImportIdentity{
                    closure_module->identity,
                    closure_module->ir_version,
                });
        }
        for (const ExactDependencyIdentity& action :
             closure_module->action_imports)
            AppendUnique(
                verified->dependency_lock.action_imports,
                action);
        for (const ExactDependencyIdentity& reducer :
             closure_module->reducer_imports)
            AppendUnique(
                verified->dependency_lock.reducer_imports,
                reducer);
        for (const SchemaIdentity& type :
             closure_module->type_imports)
        {
            AppendUnique(type_roots, type);
            AppendUnique(
                verified->dependency_lock.type_imports,
                type);
        }
        for (const CapabilityPackIdentity& pack :
             closure_module->required_capability_packs)
            AppendUnique(pack_roots, pack);
    }

    for (const ExactDependencyIdentity& action :
         verified->dependency_lock.action_imports)
    {
        const ActionDescriptor* descriptor =
            actions_.ResolveAction(action);
        if (!descriptor)
        {
            Add(
                result.diagnostics,
                VerificationErrorCode::DependencyMissing,
                "Exact action is not registered: " +
                    action.canonical_id);
        }
        else
        {
            verified->actions.push_back({action, *descriptor});
        }
    }
    for (const ExactDependencyIdentity& reducer :
         verified->dependency_lock.reducer_imports)
    {
        const ReducerDescriptor* descriptor =
            actions_.ResolveReducer(reducer);
        if (!descriptor)
        {
            Add(
                result.diagnostics,
                VerificationErrorCode::DependencyMissing,
                "Exact reducer is not registered: " +
                    reducer.canonical_id);
        }
        else
        {
            verified->reducers.push_back({reducer, *descriptor});
        }
    }

    RegistryError schema_error;
    std::optional<std::vector<TypeSchemaDefinition>> type_closure =
        schemas_.ResolveClosure(type_roots, &schema_error);
    if (!type_closure)
    {
        Add(
            result.diagnostics,
            VerificationErrorCode::DependencyMissing,
            schema_error.message);
    }
    else
    {
        verified->type_closure = std::move(*type_closure);
        for (const TypeSchemaDefinition& schema :
             verified->type_closure)
        {
            AppendUnique(
                verified->dependency_lock.type_imports,
                schema.identity);
        }
    }

    RegistryError pack_error;
    std::optional<CapabilityPackClosure> pack_closure =
        capability_packs_.ResolveClosure(
            pack_roots,
            compatibility,
            &pack_error);
    if (!pack_closure)
    {
        Add(
            result.diagnostics,
            VerificationErrorCode::CompatibilityMismatch,
            pack_error.message);
    }
    else
    {
        verified->capability_packs =
            std::move(pack_closure->dependency_order);
        for (const CapabilityPackManifest& pack :
             verified->capability_packs)
        {
            AppendUnique(
                verified->dependency_lock.capability_packs,
                pack.identity);
        }
    }

    for (const VerifiedActionBinding& action : verified->actions)
    {
        if (!Contains(
                verified->dependency_lock.capability_packs,
                action.descriptor.providing_pack))
        {
            Add(
                result.diagnostics,
                VerificationErrorCode::DependencyMismatch,
                "Action provider pack is not in the declared closure: " +
                    action.import.canonical_id);
        }
    }
    for (const VerifiedReducerBinding& reducer : verified->reducers)
    {
        if (!Contains(
                verified->dependency_lock.capability_packs,
                reducer.descriptor.providing_pack))
        {
            Add(
                result.diagnostics,
                VerificationErrorCode::DependencyMismatch,
                "Reducer provider pack is not in the declared closure: " +
                    reducer.import.canonical_id);
        }
    }
    SortLock(verified->dependency_lock);

    if (!result.diagnostics.empty())
        return result;

    const VerifiedModuleCacheKey cache_key{
        identity,
        verified->dependency_lock,
    };
    if (std::shared_ptr<const VerifiedProgramModule> cached =
            modules_.FindVerified(cache_key))
    {
        result.success = true;
        result.verified = std::move(cached);
        return result;
    }

    if (!BudgetsValid(module->budgets))
    {
        Add(
            result.diagnostics,
            VerificationErrorCode::InvalidBudget,
            "Module requires finite nonzero budgets on every axis");
    }

    ValidateSourceMap(*module, result.diagnostics);
    ValidateEffects(*module, actions_, result.diagnostics);

    SchemaLookup schema_lookup{*module, schemas_};
    std::set<SchemaIdentity> local_ids;
    for (const TypeSchemaDefinition& local : module->local_types)
    {
        if (!local_ids.emplace(local.identity).second ||
            schemas_.Resolve(
                local.identity.canonical_id,
                local.identity.version))
        {
            Add(
                result.diagnostics,
                VerificationErrorCode::InvalidSchema,
                "Local schema is duplicated or conflicts with registry: " +
                    local.identity.canonical_id);
        }
    }
    ValidateLocalSchemas(
        *module,
        schema_lookup,
        result.diagnostics);

    std::set<ProgramFunctionId> function_ids;
    std::set<std::string> function_names;
    for (const ProgramFunction& function : module->functions)
    {
        if (!function_ids.emplace(function.id).second ||
            function.name.empty() ||
            !function_names.emplace(function.name).second)
        {
            Add(
                result.diagnostics,
                VerificationErrorCode::DuplicateIdentity,
                "Function identities and names must be unique");
        }
        ValidateFunction(
            *module,
            function,
            schema_lookup,
            actions_,
            modules_,
            result.diagnostics);
    }

    std::set<std::string> entrypoint_names;
    for (const ProgramEntrypoint& entrypoint :
         module->entrypoints)
    {
        const ProgramFunction* function =
            FindFunction(*module, entrypoint.function);
        if (entrypoint.name.empty() ||
            !entrypoint_names.emplace(entrypoint.name).second ||
            !function ||
            !function->exported ||
            function->arguments.size() != 1 ||
            function->arguments.front().type !=
                entrypoint.input_type ||
            function->output_type != entrypoint.output_type ||
            !function->domain_outcome_type ||
            *function->domain_outcome_type !=
                entrypoint.domain_outcome_type ||
            !schema_lookup.Resolve(entrypoint.input_type) ||
            !schema_lookup.Resolve(entrypoint.output_type) ||
            !schema_lookup.Resolve(entrypoint.domain_outcome_type))
        {
            Add(
                result.diagnostics,
                VerificationErrorCode::InvalidEntrypoint,
                "Entrypoint signature does not match one exported function");
        }
        if (!PolicySubset(
                entrypoint.accepted_policies,
                module->accepted_policies))
        {
            Add(
                result.diagnostics,
                VerificationErrorCode::InvalidPolicy,
                "Entrypoint policy exceeds module policy");
        }
        if (entrypoint.narrowed_budgets &&
            !BudgetsNarrow(
                *entrypoint.narrowed_budgets,
                module->budgets))
        {
            Add(
                result.diagnostics,
                VerificationErrorCode::InvalidBudget,
                "Entrypoint budgets must be finite and narrow module budgets");
        }
        for (const SchemaIdentity& schema :
             entrypoint.emission_schemas)
        {
            if (!schema_lookup.Resolve(schema))
            {
                Add(
                    result.diagnostics,
                    VerificationErrorCode::InvalidSchema,
                    "Entrypoint emission schema is unresolved");
            }
        }
        for (const SchemaIdentity& schema :
             entrypoint.artifact_schemas)
        {
            if (!schema_lookup.Resolve(schema))
            {
                Add(
                    result.diagnostics,
                    VerificationErrorCode::InvalidSchema,
                    "Entrypoint artifact schema is unresolved");
            }
        }
        for (const CapabilityPackIdentity& pack :
             entrypoint.required_capability_packs)
        {
            if (!Contains(module->required_capability_packs, pack))
            {
                Add(
                    result.diagnostics,
                    VerificationErrorCode::DependencyMismatch,
                    "Entrypoint capability pack is not declared by module");
            }
        }
        if (function)
        {
            ValidateEntrypointClosure(
                *module,
                entrypoint,
                schemas_,
                actions_,
                modules_,
                result.diagnostics);
        }
    }
    if (module->entrypoints.empty())
    {
        Add(
            result.diagnostics,
            VerificationErrorCode::InvalidEntrypoint,
            "Program module requires at least one entrypoint");
    }

    if (!result.diagnostics.empty())
        return result;

    const RegistryResult cached =
        modules_.PublishVerified(cache_key, verified);
    if (!cached.success)
    {
        Add(
            result.diagnostics,
            VerificationErrorCode::DependencyMismatch,
            cached.error.message);
        return result;
    }

    result.success = true;
    result.verified = std::move(verified);
    return result;
}

} // namespace savor::runtime::program
