#pragma once

#include "../Model/ProgramModel.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace savor::runtime::program::composition {

struct CompositionDiagnostic
{
    std::string code;
    std::string message;

    auto operator<=>(const CompositionDiagnostic&) const = default;
};

struct CompositionResult
{
    bool ok = false;
    std::optional<ProgramFunctionId> function;
    std::vector<CompositionDiagnostic> diagnostics;

    [[nodiscard]] explicit operator bool() const noexcept { return ok; }
};

[[nodiscard]] ContentHash256 ContractHash(std::string_view canonical_contract);

[[nodiscard]] ExactDependencyIdentity ExactDependency(
    std::string canonical_id,
    std::uint32_t version,
    std::string_view canonical_contract);

[[nodiscard]] SchemaIdentity ExactSchema(
    std::string canonical_id,
    std::uint32_t version,
    std::string_view canonical_contract);

namespace detail {

class ModuleFragmentBuilder final
{
public:
    ModuleFragmentBuilder(
        ProgramModule& module,
        std::string source_name,
        std::string semantic_root);

    [[nodiscard]] ProgramFunction& AddFunction(
        std::string name,
        std::span<const ValueDefinition> arguments,
        TypeRef output_type,
        std::optional<TypeRef> domain_outcome_type = std::nullopt,
        bool exported = false);

    [[nodiscard]] BasicBlock& AddBlock(
        ProgramFunction& function,
        std::span<const ValueDefinition> arguments = {});

    [[nodiscard]] ValueDefinition NewArgument(TypeRef type);

    [[nodiscard]] std::optional<ProgramValueId> AddInstruction(
        ProgramFunction& function,
        BasicBlock& block,
        InstructionOpcode opcode,
        std::optional<TypeRef> result_type = std::nullopt,
        std::span<const ProgramValueId> operands = {},
        InstructionTarget target = {},
        std::string selector = {},
        std::optional<LiteralValue> literal = std::nullopt,
        ProgramScopeId scope = {});

    void SetTerminator(
        ProgramFunction& function,
        BasicBlock& block,
        Terminator terminator,
        std::string semantic_suffix);

    void AddActionImport(const ExactDependencyIdentity& dependency);
    void AddReducerImport(const ExactDependencyIdentity& dependency);
    void AddTypeImport(const SchemaIdentity& schema);
    void AddTypeImport(const TypeRef& type);
    void AddCapabilityImport(const CapabilityPackIdentity& pack);
    void AddLocalType(TypeSchemaDefinition definition);

    [[nodiscard]] ProgramScopeId NewScope() noexcept;
    [[nodiscard]] ProgramValueId NewValue() noexcept;

private:
    [[nodiscard]] ProgramSourceLocationId NewSourceLocation(
        ProgramFunctionId function,
        ProgramBlockId block,
        std::optional<ProgramInstructionId> instruction,
        std::string semantic_suffix);

    ProgramModule& module_;
    std::string source_name_;
    std::string semantic_root_;
    std::uint64_t next_function_ = 1;
    std::uint64_t next_block_ = 1;
    std::uint64_t next_value_ = 1;
    std::uint64_t next_instruction_ = 1;
    std::uint64_t next_source_location_ = 1;
    std::uint64_t next_scope_ = 1;
};

[[nodiscard]] CompositionResult Fail(
    std::string code,
    std::string message);

} // namespace detail
} // namespace savor::runtime::program::composition
