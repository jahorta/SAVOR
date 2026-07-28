#pragma once

#include "Runner/Runtime/ProgramRuntime/Model/ProgramModel.h"
#include "Runner/Runtime/ProgramRuntime/Registry/RegistryResult.h"

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace savor::runtime::program {

struct VerifiedProgramModule;

struct ModuleStoreResult
{
    bool success = false;
    std::shared_ptr<const ProgramModule> module;
    RegistryError error;
};

struct ModuleDependencyClosure
{
    std::vector<std::shared_ptr<const ProgramModule>> dependency_order;
};

struct VerifiedModuleCacheKey
{
    ModuleIdentity module;
    ProgramDependencyLock dependency_lock;

    auto operator<=>(const VerifiedModuleCacheKey&) const = default;
};

class ProgramDefinitionStore final
{
public:
    [[nodiscard]] ModuleStoreResult RegisterCompiled(
        ProgramModule module);
    [[nodiscard]] RegistryResult RegisterCompiledBatch(
        std::vector<ProgramModule> modules);
    [[nodiscard]] ModuleStoreResult RegisterEncoded(
        std::span<const Byte> payload,
        const std::optional<ModuleIdentity>& expected_identity =
            std::nullopt);

    [[nodiscard]] std::shared_ptr<const ProgramModule> Resolve(
        const ModuleIdentity& identity) const noexcept;
    [[nodiscard]] std::optional<ModuleDependencyClosure> ResolveClosure(
        const ModuleIdentity& root,
        RegistryError* error = nullptr) const;

    [[nodiscard]] std::shared_ptr<const VerifiedProgramModule>
        FindVerified(const VerifiedModuleCacheKey& key) const noexcept;
    [[nodiscard]] RegistryResult PublishVerified(
        VerifiedModuleCacheKey key,
        std::shared_ptr<const VerifiedProgramModule> verified);

    [[nodiscard]] std::size_t size() const noexcept
    {
        return modules_.size();
    }
    [[nodiscard]] std::size_t verified_cache_size() const noexcept
    {
        return verified_.size();
    }

private:
    using Key = std::pair<std::string, std::uint32_t>;
    using ModuleMap =
        std::map<Key, std::shared_ptr<const ProgramModule>>;

    [[nodiscard]] static RegistryResult ValidateCandidate(
        const ModuleMap& modules);

    ModuleMap modules_;
    std::map<
        VerifiedModuleCacheKey,
        std::shared_ptr<const VerifiedProgramModule>>
        verified_;
};

} // namespace savor::runtime::program
