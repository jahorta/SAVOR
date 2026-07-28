#include "CompositionSupport.h"

#include "Utils/Hash.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace savor::runtime::program::composition {
namespace {

template <typename Identity, typename Projection>
void AppendUniqueSorted(
    std::vector<Identity>& values,
    const Identity& value,
    Projection projection)
{
    const auto needle = projection(value);
    const auto existing = std::ranges::find_if(
        values,
        [&](const Identity& candidate)
        {
            return projection(candidate) == needle;
        });
    if (existing == values.end())
    {
        values.push_back(value);
        std::ranges::sort(
            values,
            {},
            [&](const Identity& candidate)
            {
                return projection(candidate);
            });
    }
}

template <typename Id>
void AdvancePast(std::uint64_t& next, Id id)
{
    if (id.value() >= next && id.value() != std::numeric_limits<std::uint64_t>::max())
        next = id.value() + 1;
}

} // namespace

ContentHash256 ContractHash(std::string_view canonical_contract)
{
    const auto hex = hash::sha256(
        canonical_contract.data(),
        canonical_contract.size());
    return ContentHash256::FromHex(hex).value_or(ContentHash256{});
}

ExactDependencyIdentity ExactDependency(
    std::string canonical_id,
    std::uint32_t version,
    std::string_view canonical_contract)
{
    return {
        .canonical_id = std::move(canonical_id),
        .version = version,
        .signature_hash = ContractHash(canonical_contract),
    };
}

SchemaIdentity ExactSchema(
    std::string canonical_id,
    std::uint32_t version,
    std::string_view canonical_contract)
{
    return {
        .canonical_id = std::move(canonical_id),
        .version = version,
        .schema_hash = ContractHash(canonical_contract),
    };
}

namespace detail {

ModuleFragmentBuilder::ModuleFragmentBuilder(
    ProgramModule& module,
    std::string source_name,
    std::string semantic_root)
    : module_(module),
      source_name_(std::move(source_name)),
      semantic_root_(std::move(semantic_root))
{
    for (const auto& function : module_.functions)
    {
        AdvancePast(next_function_, function.id);
        for (const auto& argument : function.arguments)
            AdvancePast(next_value_, argument.id);
        for (const auto& block : function.blocks)
        {
            AdvancePast(next_block_, block.id);
            for (const auto& argument : block.arguments)
                AdvancePast(next_value_, argument.id);
            for (const auto& instruction : block.instructions)
            {
                AdvancePast(next_instruction_, instruction.id);
                AdvancePast(next_source_location_, instruction.source_location);
                AdvancePast(next_scope_, instruction.scope);
                if (instruction.result)
                    AdvancePast(next_value_, instruction.result->id);
            }
            AdvancePast(next_source_location_, block.terminator.source_location);
        }
    }
    for (const auto& entry : module_.source_map.entries)
        AdvancePast(next_source_location_, entry.id);
}

ProgramFunction& ModuleFragmentBuilder::AddFunction(
    std::string name,
    std::span<const ValueDefinition> arguments,
    TypeRef output_type,
    std::optional<TypeRef> domain_outcome_type,
    bool exported)
{
    ProgramFunction function{};
    function.id = ProgramFunctionId(next_function_++);
    function.name = std::move(name);
    function.arguments.assign(arguments.begin(), arguments.end());
    function.output_type = std::move(output_type);
    function.domain_outcome_type = std::move(domain_outcome_type);
    function.exported = exported;
    module_.functions.push_back(std::move(function));
    return module_.functions.back();
}

BasicBlock& ModuleFragmentBuilder::AddBlock(
    ProgramFunction& function,
    std::span<const ValueDefinition> arguments)
{
    BasicBlock block{};
    block.id = ProgramBlockId(next_block_++);
    block.arguments.assign(arguments.begin(), arguments.end());
    function.blocks.push_back(std::move(block));
    if (!function.entry_block)
        function.entry_block = function.blocks.back().id;
    return function.blocks.back();
}

ValueDefinition ModuleFragmentBuilder::NewArgument(TypeRef type)
{
    return {
        .id = ProgramValueId(next_value_++),
        .type = std::move(type),
    };
}

std::optional<ProgramValueId> ModuleFragmentBuilder::AddInstruction(
    ProgramFunction& function,
    BasicBlock& block,
    InstructionOpcode opcode,
    std::optional<TypeRef> result_type,
    std::span<const ProgramValueId> operands,
    InstructionTarget target,
    std::string selector,
    std::optional<LiteralValue> literal,
    ProgramScopeId scope)
{
    Instruction instruction{};
    instruction.id = ProgramInstructionId(next_instruction_++);
    instruction.opcode = opcode;
    instruction.source_location = NewSourceLocation(
        function.id,
        block.id,
        instruction.id,
        selector.empty() ? "instruction" : selector);
    if (result_type)
    {
        instruction.result = ValueDefinition{
            .id = ProgramValueId(next_value_++),
            .type = std::move(*result_type),
        };
    }
    instruction.operands.assign(operands.begin(), operands.end());
    instruction.literal = std::move(literal);
    instruction.target = std::move(target);
    instruction.selector = std::move(selector);
    instruction.ordinal = block.instructions.size();
    instruction.scope = scope;
    const auto result = instruction.result
        ? std::optional<ProgramValueId>(instruction.result->id)
        : std::nullopt;
    block.instructions.push_back(std::move(instruction));
    return result;
}

void ModuleFragmentBuilder::SetTerminator(
    ProgramFunction& function,
    BasicBlock& block,
    Terminator terminator,
    std::string semantic_suffix)
{
    terminator.source_location = NewSourceLocation(
        function.id,
        block.id,
        std::nullopt,
        std::move(semantic_suffix));
    block.terminator = std::move(terminator);
}

void ModuleFragmentBuilder::AddActionImport(
    const ExactDependencyIdentity& dependency)
{
    AppendUniqueSorted(
        module_.action_imports,
        dependency,
        [](const ExactDependencyIdentity& value)
        {
            return std::pair{value.canonical_id, value.version};
        });
}

void ModuleFragmentBuilder::AddReducerImport(
    const ExactDependencyIdentity& dependency)
{
    AppendUniqueSorted(
        module_.reducer_imports,
        dependency,
        [](const ExactDependencyIdentity& value)
        {
            return std::pair{value.canonical_id, value.version};
        });
}

void ModuleFragmentBuilder::AddTypeImport(const SchemaIdentity& schema)
{
    AppendUniqueSorted(
        module_.type_imports,
        schema,
        [](const SchemaIdentity& value)
        {
            return std::pair{value.canonical_id, value.version};
        });
}

void ModuleFragmentBuilder::AddTypeImport(const TypeRef& type)
{
    if (type.is_named())
        AddTypeImport(*type.named);
}

void ModuleFragmentBuilder::AddCapabilityImport(const CapabilityPackIdentity& pack)
{
    AppendUniqueSorted(
        module_.required_capability_packs,
        pack,
        [](const CapabilityPackIdentity& value)
        {
            return std::pair{value.canonical_id, value.version};
        });
}

void ModuleFragmentBuilder::AddLocalType(TypeSchemaDefinition definition)
{
    const auto existing = std::ranges::find_if(
        module_.local_types,
        [&](const TypeSchemaDefinition& candidate)
        {
            return candidate.identity.canonical_id ==
                    definition.identity.canonical_id &&
                candidate.identity.version == definition.identity.version;
        });
    if (existing == module_.local_types.end())
    {
        module_.local_types.push_back(std::move(definition));
        std::ranges::sort(
            module_.local_types,
            {},
            [](const TypeSchemaDefinition& value)
            {
                return std::pair{
                    value.identity.canonical_id,
                    value.identity.version};
            });
    }
}

ProgramScopeId ModuleFragmentBuilder::NewScope() noexcept
{
    return ProgramScopeId(next_scope_++);
}

ProgramValueId ModuleFragmentBuilder::NewValue() noexcept
{
    return ProgramValueId(next_value_++);
}

ProgramSourceLocationId ModuleFragmentBuilder::NewSourceLocation(
    ProgramFunctionId function,
    ProgramBlockId block,
    std::optional<ProgramInstructionId> instruction,
    std::string semantic_suffix)
{
    const auto id = ProgramSourceLocationId(next_source_location_++);
    module_.source_map.entries.push_back({
        .id = id,
        .function = function,
        .block = block,
        .instruction = instruction,
        .source_name = source_name_,
        .semantic_path = semantic_root_ + "/" + semantic_suffix,
    });
    return id;
}

CompositionResult Fail(std::string code, std::string message)
{
    return {
        .ok = false,
        .function = std::nullopt,
        .diagnostics = {{
            .code = std::move(code),
            .message = std::move(message),
        }},
    };
}

} // namespace detail
} // namespace savor::runtime::program::composition
