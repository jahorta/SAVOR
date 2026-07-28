#include "ProgramDefinitionStore.h"

#include "Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"

#include <functional>

namespace savor::runtime::program {
namespace {

using Key = std::pair<std::string, std::uint32_t>;

Key MakeKey(const ModuleIdentity& identity)
{
    return {identity.canonical_id, identity.revision};
}

bool Valid(const ModuleIdentity& identity)
{
    return !identity.canonical_id.empty() &&
        identity.revision != 0 &&
        !identity.module_hash.empty();
}

RegistryResult Failure(
    RegistryErrorCode code,
    const std::string& message)
{
    return RegistryResult::Failure(code, message);
}

} // namespace

ModuleStoreResult ProgramDefinitionStore::RegisterCompiled(
    ProgramModule module)
{
    const ModuleIdentity identity = module.identity;
    std::vector<ProgramModule> batch;
    batch.push_back(std::move(module));
    const RegistryResult result =
        RegisterCompiledBatch(std::move(batch));
    if (!result.success)
        return {false, {}, result.error};
    return {true, Resolve(identity), {}};
}

RegistryResult ProgramDefinitionStore::RegisterCompiledBatch(
    std::vector<ProgramModule> modules)
{
    ModuleMap candidate = modules_;
    for (ProgramModule& module : modules)
    {
        if (!Valid(module.identity))
        {
            return Failure(
                RegistryErrorCode::InvalidIdentity,
                "Module requires canonical id, revision, and hash");
        }
        if (module.ir_version != kCanonicalIrVersionV1)
        {
            return Failure(
                RegistryErrorCode::InvalidArgument,
                "ProgramDefinitionStore supports canonical IR version 1");
        }
        const CodecStatus identity =
            ValidateProgramModuleIdentityV1(module);
        if (!identity)
        {
            return Failure(
                RegistryErrorCode::ReferenceMismatch,
                "Module canonical identity is invalid: " +
                    identity.message);
        }

        const Key key = MakeKey(module.identity);
        const auto found = candidate.find(key);
        if (found != candidate.end())
        {
            if (*found->second == module)
                continue;
            return Failure(
                RegistryErrorCode::IdentityConflict,
                "Module id/revision already has different content: " +
                    module.identity.canonical_id);
        }
        candidate.emplace(
            key,
            std::make_shared<const ProgramModule>(std::move(module)));
    }

    const RegistryResult validation = ValidateCandidate(candidate);
    if (!validation.success)
        return validation;

    modules_ = std::move(candidate);
    return RegistryResult::Success();
}

ModuleStoreResult ProgramDefinitionStore::RegisterEncoded(
    std::span<const Byte> payload,
    const std::optional<ModuleIdentity>& expected_identity)
{
    DecodeResult<ProgramModule> decoded =
        DecodeProgramModuleV1(payload);
    if (!decoded)
    {
        return {
            false,
            {},
            {
                RegistryErrorCode::InvalidArgument,
                "Encoded module rejected: " + decoded.status.message,
            },
        };
    }
    if (expected_identity &&
        decoded.value->identity != *expected_identity)
    {
        return {
            false,
            {},
            {
                RegistryErrorCode::ReferenceMismatch,
                "Encoded module does not match expected exact identity",
            },
        };
    }
    return RegisterCompiled(std::move(*decoded.value));
}

std::shared_ptr<const ProgramModule> ProgramDefinitionStore::Resolve(
    const ModuleIdentity& identity) const noexcept
{
    const auto found = modules_.find(MakeKey(identity));
    if (found == modules_.end() ||
        found->second->identity != identity)
    {
        return {};
    }
    return found->second;
}

std::optional<ModuleDependencyClosure>
ProgramDefinitionStore::ResolveClosure(
    const ModuleIdentity& root,
    RegistryError* error) const
{
    enum class Visit : std::uint8_t
    {
        Visiting,
        Complete,
    };
    std::map<Key, Visit> visits;
    ModuleDependencyClosure closure;

    std::function<bool(const ModuleIdentity&)> visit =
        [&](const ModuleIdentity& identity) -> bool {
        std::shared_ptr<const ProgramModule> module = Resolve(identity);
        if (!module)
        {
            if (error)
            {
                *error = {
                    RegistryErrorCode::DependencyMissing,
                    "Exact module dependency is not registered: " +
                        identity.canonical_id,
                };
            }
            return false;
        }
        const Key key = MakeKey(identity);
        const auto prior = visits.find(key);
        if (prior != visits.end())
        {
            if (prior->second == Visit::Visiting)
            {
                if (error)
                {
                    *error = {
                        RegistryErrorCode::DependencyCycle,
                        "Module import cycle: " +
                            identity.canonical_id,
                    };
                }
                return false;
            }
            return true;
        }

        visits.emplace(key, Visit::Visiting);
        for (const ModuleImportIdentity& imported :
             module->module_imports)
        {
            if (!visit(imported.module))
                return false;
        }
        visits[key] = Visit::Complete;
        closure.dependency_order.push_back(std::move(module));
        return true;
    };

    if (!visit(root))
        return std::nullopt;
    if (error)
        *error = {};
    return closure;
}

std::shared_ptr<const VerifiedProgramModule>
ProgramDefinitionStore::FindVerified(
    const VerifiedModuleCacheKey& key) const noexcept
{
    const auto found = verified_.find(key);
    return found == verified_.end() ? nullptr : found->second;
}

RegistryResult ProgramDefinitionStore::PublishVerified(
    VerifiedModuleCacheKey key,
    std::shared_ptr<const VerifiedProgramModule> verified)
{
    if (!verified || !Resolve(key.module))
    {
        return Failure(
            RegistryErrorCode::InvalidArgument,
            "Verified cache requires a registered module and value");
    }
    const auto found = verified_.find(key);
    if (found != verified_.end())
    {
        if (found->second == verified)
            return RegistryResult::Success();
        return Failure(
            RegistryErrorCode::IdentityConflict,
            "Verified dependency closure is already cached");
    }
    verified_.emplace(std::move(key), std::move(verified));
    return RegistryResult::Success();
}

RegistryResult ProgramDefinitionStore::ValidateCandidate(
    const ModuleMap& modules)
{
    enum class Visit : std::uint8_t
    {
        Visiting,
        Complete,
    };
    std::map<Key, Visit> visits;
    std::function<RegistryResult(const ProgramModule&)> visit =
        [&](const ProgramModule& module) -> RegistryResult {
        const Key key = MakeKey(module.identity);
        const auto prior = visits.find(key);
        if (prior != visits.end())
        {
            if (prior->second == Visit::Visiting)
            {
                return Failure(
                    RegistryErrorCode::DependencyCycle,
                    "Module import cycle: " +
                        module.identity.canonical_id);
            }
            return RegistryResult::Success();
        }
        visits.emplace(key, Visit::Visiting);

        for (const ModuleImportIdentity& imported :
             module.module_imports)
        {
            const auto found = modules.find(MakeKey(imported.module));
            if (found == modules.end())
            {
                return Failure(
                    RegistryErrorCode::DependencyMissing,
                    "Module import is not registered: " +
                        imported.module.canonical_id);
            }
            if (found->second->identity != imported.module ||
                found->second->ir_version != imported.ir_version)
            {
                return Failure(
                    RegistryErrorCode::ReferenceMismatch,
                    "Module import exact identity or IR version mismatch: " +
                        imported.module.canonical_id);
            }
            const RegistryResult nested = visit(*found->second);
            if (!nested.success)
                return nested;
        }
        visits[key] = Visit::Complete;
        return RegistryResult::Success();
    };

    for (const auto& [key, module] : modules)
    {
        (void)key;
        const RegistryResult result = visit(*module);
        if (!result.success)
            return result;
    }
    return RegistryResult::Success();
}

} // namespace savor::runtime::program
