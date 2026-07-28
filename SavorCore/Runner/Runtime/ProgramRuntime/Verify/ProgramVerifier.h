#pragma once

#include "Runner/Runtime/ProgramRuntime/Model/ProgramModel.h"
#include "Runner/Runtime/ProgramRuntime/Registry/ActionRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CapabilityPackRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Registry/TypeSchemaRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Store/ProgramDefinitionStore.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace savor::runtime::program {

enum class VerificationErrorCode : std::uint16_t
{
    None,
    ModuleNotFound,
    CanonicalIdentity,
    UnsupportedIrVersion,
    DependencyMissing,
    DependencyMismatch,
    CompatibilityMismatch,
    InvalidSchema,
    InvalidSourceMap,
    DuplicateIdentity,
    InvalidEntrypoint,
    InvalidFunction,
    InvalidControlFlow,
    InvalidTerminator,
    UndefinedValue,
    DefinitionDoesNotDominateUse,
    TypeMismatch,
    InvalidInstruction,
    UndeclaredEffect,
    InvalidScope,
    InvalidBudget,
    InvalidPolicy,
};

struct VerificationDiagnostic
{
    VerificationErrorCode code = VerificationErrorCode::None;
    std::string message;
    std::optional<ProgramSourceLocationId> source_location;

    auto operator<=>(const VerificationDiagnostic&) const = default;
};

struct VerifiedActionBinding
{
    ExactDependencyIdentity import;
    ActionDescriptor descriptor;
};

struct VerifiedReducerBinding
{
    ExactDependencyIdentity import;
    ReducerDescriptor descriptor;
};

struct VerifiedProgramModule
{
    std::shared_ptr<const ProgramModule> module;
    ProgramDependencyLock dependency_lock;
    std::vector<std::shared_ptr<const ProgramModule>> module_closure;
    std::vector<TypeSchemaDefinition> type_closure;
    std::vector<VerifiedActionBinding> actions;
    std::vector<VerifiedReducerBinding> reducers;
    std::vector<CapabilityPackManifest> capability_packs;
};

struct ProgramVerificationResult
{
    bool success = false;
    std::shared_ptr<const VerifiedProgramModule> verified;
    std::vector<VerificationDiagnostic> diagnostics;
};

class ProgramVerifier final
{
public:
    ProgramVerifier(
        ProgramDefinitionStore& modules,
        const TypeSchemaRegistry& schemas,
        const ActionRegistry& actions,
        const CapabilityPackRegistry& capability_packs) noexcept
        : modules_(modules),
          schemas_(schemas),
          actions_(actions),
          capability_packs_(capability_packs)
    {
    }

    [[nodiscard]] ProgramVerificationResult Verify(
        const ModuleIdentity& module,
        const RuntimeCompatibility& compatibility);

private:
    ProgramDefinitionStore& modules_;
    const TypeSchemaRegistry& schemas_;
    const ActionRegistry& actions_;
    const CapabilityPackRegistry& capability_packs_;
};

} // namespace savor::runtime::program
