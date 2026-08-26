#include "CapabilityPackRegistry.h"

#include "Utils/Hash.h"

#include <algorithm>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace savor::runtime::program {
namespace {

using Key = std::pair<std::string, std::uint32_t>;

Key MakeKey(const CapabilityPackIdentity& identity)
{
    return {identity.canonical_id, identity.version};
}

bool Valid(const CapabilityPackIdentity& identity)
{
    return !identity.canonical_id.empty() &&
        identity.version != 0 &&
        !identity.manifest_hash.empty();
}

bool ExactMatch(
    const CapabilityPackIdentity& lhs,
    const CapabilityPackIdentity& rhs)
{
    return lhs == rhs;
}

RegistryResult Failure(
    RegistryErrorCode code,
    const std::string& message)
{
    return RegistryResult::Failure(code, message);
}

template <typename Range, typename Name>
bool UniqueNames(const Range& range, Name name)
{
    std::set<std::string> names;
    for (const auto& item : range)
    {
        const std::string& value = name(item);
        if (value.empty() || !names.emplace(value).second)
            return false;
    }
    return true;
}

void AppendString(std::string& contract, std::string_view value)
{
    contract.append(std::to_string(value.size()));
    contract.push_back(':');
    contract.append(value);
    contract.push_back(';');
}

template <typename Value>
void AppendNumber(std::string& contract, Value value)
{
    contract.append(std::to_string(
        static_cast<std::uint64_t>(value)));
    contract.push_back(';');
}

void AppendHash(
    std::string& contract,
    const ContentHash256& value)
{
    contract.append(value.ToHex());
    contract.push_back(';');
}

void AppendType(std::string& contract, const TypeRef& type)
{
    AppendNumber(contract, type.builtin);
    AppendNumber(contract, type.named.has_value());
    if (!type.named)
        return;
    AppendString(contract, type.named->canonical_id);
    AppendNumber(contract, type.named->version);
    AppendHash(contract, type.named->schema_hash);
}

void AppendPackIdentity(
    std::string& contract,
    const CapabilityPackIdentity& identity)
{
    AppendString(contract, identity.canonical_id);
    AppendNumber(contract, identity.version);
    AppendHash(contract, identity.manifest_hash);
}

void AppendSchemaIdentity(
    std::string& contract,
    const SchemaIdentity& identity)
{
    AppendString(contract, identity.canonical_id);
    AppendNumber(contract, identity.version);
    AppendHash(contract, identity.schema_hash);
}

void AppendDependencyIdentity(
    std::string& contract,
    const ExactDependencyIdentity& identity)
{
    AppendString(contract, identity.canonical_id);
    AppendNumber(contract, identity.version);
    AppendHash(contract, identity.signature_hash);
}

template <typename Range, typename Serialize>
void AppendNormalized(
    std::string& contract,
    const Range& range,
    Serialize serialize)
{
    std::vector<std::string> entries;
    entries.reserve(range.size());
    for (const auto& item : range)
    {
        std::string entry;
        serialize(entry, item);
        entries.push_back(std::move(entry));
    }
    std::ranges::sort(entries);
    AppendNumber(contract, entries.size());
    for (const std::string& entry : entries)
        AppendString(contract, entry);
}

ContentHash256 ContractHash(std::string_view contract)
{
    const std::string digest =
        hash::sha256(contract.data(), contract.size());
    const auto result = ContentHash256::FromHex(digest);
    if (!result)
        throw std::logic_error("Capability pack contract hash failed");
    return *result;
}

} // namespace

ContentHash256 ComputeCapabilityPackManifestContractHash(
    const CapabilityPackManifest& manifest)
{
    std::string contract("capability-pack-manifest/1;");
    AppendString(contract, manifest.identity.canonical_id);
    AppendNumber(contract, manifest.identity.version);
    AppendString(contract, manifest.compatibility.game_id);
    AppendString(
        contract,
        manifest.compatibility.executable_identity);
    AppendString(
        contract,
        manifest.compatibility.address_map_revision);

    AppendNormalized(
        contract,
        manifest.dependencies,
        [](std::string& entry, const CapabilityPackIdentity& value)
        {
            AppendPackIdentity(entry, value);
        });
    AppendNormalized(
        contract,
        manifest.schemas,
        [](std::string& entry, const SchemaIdentity& value)
        {
            AppendSchemaIdentity(entry, value);
        });
    AppendNormalized(
        contract,
        manifest.semantic_points,
        [](std::string& entry, const SemanticPointDescriptor& point)
        {
            AppendString(entry, point.canonical_id);
            AppendNumber(entry, point.kind);
            AppendNumber(entry, point.pc);
            AppendNumber(entry, point.memory_address);
            AppendNumber(entry, point.memory_size);
            AppendNumber(entry, point.memory_read);
            AppendNumber(entry, point.memory_write);
            AppendString(entry, point.synthetic_identity);
        });
    AppendNormalized(
        contract,
        manifest.address_symbols,
        [](std::string& entry, const AddressSymbolDescriptor& symbol)
        {
            AppendString(entry, symbol.canonical_id);
            AppendNumber(entry, symbol.address);
            AppendType(entry, symbol.value_type);
        });
    AppendNormalized(
        contract,
        manifest.coherent_queries,
        [](std::string& entry, const CoherentQueryDescriptor& query)
        {
            AppendString(entry, query.canonical_id);
            AppendSchemaIdentity(entry, query.result_schema);
            auto dependencies = query.address_dependencies;
            std::ranges::sort(dependencies);
            AppendNumber(entry, dependencies.size());
            for (const std::string& dependency : dependencies)
                AppendString(entry, dependency);
            AppendNumber(entry, query.maximum_guest_reads);
        });
    AppendNormalized(
        contract,
        manifest.cpu_evaluators,
        [](std::string& entry, const CpuEvaluatorDescriptor& evaluator)
        {
            AppendString(entry, evaluator.canonical_id);
            AppendNumber(entry, evaluator.routed_sample_descriptor_id);
            AppendNumber(entry, evaluator.source);
            AppendString(entry, evaluator.address_dependency);
            AppendType(entry, evaluator.result_type);
            AppendNumber(entry, evaluator.operations.size());
            for (CpuEvaluatorOperation operation :
                 evaluator.operations)
            {
                AppendNumber(entry, operation);
            }
            AppendNumber(entry, evaluator.maximum_reads);
            AppendNumber(entry, evaluator.maximum_output_bytes);
        });
    AppendNormalized(
        contract,
        manifest.actions,
        [](std::string& entry, const ExactDependencyIdentity& action)
        {
            AppendDependencyIdentity(entry, action);
        });
    AppendNormalized(
        contract,
        manifest.reducers,
        [](std::string& entry, const ExactDependencyIdentity& reducer)
        {
            AppendDependencyIdentity(entry, reducer);
        });
    return ContractHash(contract);
}

RegistryResult CapabilityPackRegistry::Register(
    CapabilityPackManifest manifest)
{
    std::vector<CapabilityPackManifest> batch;
    batch.push_back(std::move(manifest));
    return RegisterBatch(std::move(batch));
}

RegistryResult CapabilityPackRegistry::RegisterBatch(
    std::vector<CapabilityPackManifest> manifests)
{
    ManifestMap candidate = manifests_;
    for (CapabilityPackManifest& manifest : manifests)
    {
        const RegistryResult shape = ValidateManifestShape(manifest);
        if (!shape.success)
            return shape;

        const Key key = MakeKey(manifest.identity);
        const auto found = candidate.find(key);
        if (found != candidate.end())
        {
            if (found->second == manifest)
                continue;
            return Failure(
                RegistryErrorCode::IdentityConflict,
                "Capability pack identity already has different content: " +
                    manifest.identity.canonical_id);
        }
        candidate.emplace(key, std::move(manifest));
    }

    const RegistryResult validation = ValidateCandidate(candidate);
    if (!validation.success)
        return validation;

    manifests_ = std::move(candidate);
    return RegistryResult::Success();
}

const CapabilityPackManifest* CapabilityPackRegistry::Resolve(
    const CapabilityPackIdentity& identity) const noexcept
{
    const auto found = manifests_.find(MakeKey(identity));
    if (found == manifests_.end() ||
        !ExactMatch(found->second.identity, identity))
    {
        return nullptr;
    }
    return &found->second;
}

std::optional<CapabilityPackClosure>
CapabilityPackRegistry::ResolveClosure(
    const std::vector<CapabilityPackIdentity>& roots,
    const RuntimeCompatibility& compatibility,
    RegistryError* error) const
{
    enum class Visit : std::uint8_t
    {
        Visiting,
        Complete,
    };
    std::map<Key, Visit> visits;
    CapabilityPackClosure closure;

    std::function<bool(const CapabilityPackIdentity&)> visit =
        [&](const CapabilityPackIdentity& identity) -> bool {
        const CapabilityPackManifest* manifest = Resolve(identity);
        if (!manifest)
        {
            if (error)
            {
                *error = {
                    RegistryErrorCode::DependencyMissing,
                    "Exact capability pack is not registered: " +
                        identity.canonical_id,
                };
            }
            return false;
        }
        if (manifest->compatibility != compatibility)
        {
            if (error)
            {
                *error = {
                    RegistryErrorCode::CompatibilityMismatch,
                    "Capability pack is incompatible with runtime: " +
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
                        "Capability pack dependency cycle: " +
                            identity.canonical_id,
                    };
                }
                return false;
            }
            return true;
        }

        visits.emplace(key, Visit::Visiting);
        for (const CapabilityPackIdentity& dependency :
             manifest->dependencies)
        {
            if (!visit(dependency))
                return false;
        }
        visits[key] = Visit::Complete;
        closure.dependency_order.push_back(*manifest);
        return true;
    };

    for (const CapabilityPackIdentity& root : roots)
    {
        if (!visit(root))
            return std::nullopt;
    }
    if (error)
        *error = {};
    return closure;
}

RegistryResult CapabilityPackRegistry::ValidateManifestShape(
    const CapabilityPackManifest& manifest) const
{
    if (!Valid(manifest.identity))
    {
        return Failure(
            RegistryErrorCode::InvalidIdentity,
            "Capability pack requires canonical id, version, and hash");
    }
    if (manifest.compatibility.game_id.empty() ||
        manifest.compatibility.executable_identity.empty() ||
        manifest.compatibility.address_map_revision.empty())
    {
        return Failure(
            RegistryErrorCode::InvalidArgument,
            "Capability pack requires exact runtime compatibility");
    }
    if (!UniqueNames(
            manifest.semantic_points,
            [](const SemanticPointDescriptor& point)
                -> const std::string& { return point.canonical_id; }) ||
        !UniqueNames(
            manifest.address_symbols,
            [](const AddressSymbolDescriptor& symbol)
                -> const std::string& { return symbol.canonical_id; }) ||
        !UniqueNames(
            manifest.coherent_queries,
            [](const CoherentQueryDescriptor& query)
                -> const std::string& { return query.canonical_id; }) ||
        !UniqueNames(
            manifest.cpu_evaluators,
            [](const CpuEvaluatorDescriptor& evaluator)
                -> const std::string& { return evaluator.canonical_id; }))
    {
        return Failure(
            RegistryErrorCode::InvalidArgument,
            "Capability pack entries require unique nonempty identities");
    }

    for (const SemanticPointDescriptor& point : manifest.semantic_points)
    {
        switch (point.kind)
        {
        case SemanticPointKind::ProgramCounter:
            if (point.pc == 0)
            {
                return Failure(
                    RegistryErrorCode::InvalidArgument,
                    "Program-counter semantic points cannot use address zero");
            }
            break;
        case SemanticPointKind::Memory:
            if (point.memory_address == 0 ||
                point.memory_size == 0 ||
                (!point.memory_read && !point.memory_write))
            {
                return Failure(
                    RegistryErrorCode::InvalidArgument,
                    "Memory semantic points require a range and access");
            }
            break;
        case SemanticPointKind::Synthetic:
            if (point.synthetic_identity.empty())
            {
                return Failure(
                    RegistryErrorCode::InvalidArgument,
                    "Synthetic semantic point requires physical identity");
            }
            break;
        }
    }

    std::set<std::string> symbol_names;
    for (const AddressSymbolDescriptor& symbol : manifest.address_symbols)
    {
        symbol_names.emplace(symbol.canonical_id);
        if (symbol.address == 0)
        {
            return Failure(
                RegistryErrorCode::InvalidArgument,
                "Address symbols cannot use address zero");
        }
        if (symbol.value_type.is_named() &&
            schemas_ &&
            !schemas_->Resolve(*symbol.value_type.named))
        {
            return Failure(
                RegistryErrorCode::DependencyMissing,
                "Address symbol references an unregistered schema");
        }
    }

    for (const CoherentQueryDescriptor& query :
         manifest.coherent_queries)
    {
        if (query.maximum_guest_reads == 0 ||
            (schemas_ && !schemas_->Resolve(query.result_schema)))
        {
            return Failure(
                RegistryErrorCode::DependencyMissing,
                "Coherent query requires a registered result schema and finite read bound");
        }
        for (const std::string& dependency : query.address_dependencies)
        {
            if (!symbol_names.contains(dependency))
            {
                return Failure(
                    RegistryErrorCode::DependencyMissing,
                    "Coherent query references an unknown address symbol");
            }
        }
    }

    std::set<std::uint32_t> routed_sample_ids;
    for (const CpuEvaluatorDescriptor& evaluator :
         manifest.cpu_evaluators)
    {
        const bool host_movie_input_count =
            evaluator.source == CpuEvaluatorSource::HostMovieInputCount;
        if (evaluator.routed_sample_descriptor_id == 0 ||
            !routed_sample_ids.emplace(
                evaluator.routed_sample_descriptor_id).second ||
            (evaluator.result_type.is_named() && schemas_ &&
                !schemas_->Resolve(*evaluator.result_type.named)) ||
            (!evaluator.result_type.is_named() &&
                evaluator.result_type.builtin == BuiltinType::Unit) ||
            evaluator.maximum_output_bytes == 0 ||
            (!host_movie_input_count &&
                (evaluator.operations.empty() ||
                 evaluator.maximum_reads == 0 ||
                 evaluator.operations.size() >
                    static_cast<std::size_t>(evaluator.maximum_reads) * 4u)) ||
            (host_movie_input_count &&
                (!evaluator.address_dependency.empty() ||
                 !evaluator.operations.empty() ||
                 evaluator.maximum_reads != 0 ||
                 evaluator.result_type.is_named() ||
                 evaluator.result_type.builtin != BuiltinType::U64 ||
                 evaluator.maximum_output_bytes != 8)))
        {
            return Failure(
                RegistryErrorCode::InvalidArgument,
                "CPU evaluator must be bounded and nonempty");
        }
        if (!evaluator.address_dependency.empty() &&
            !symbol_names.contains(evaluator.address_dependency))
        {
            return Failure(
                RegistryErrorCode::DependencyMissing,
                "CPU evaluator references an unknown address symbol");
        }
    }

    if (schemas_)
    {
        for (const SchemaIdentity& schema : manifest.schemas)
        {
            if (!schemas_->Resolve(schema))
            {
                return Failure(
                    RegistryErrorCode::DependencyMissing,
                    "Capability pack references an unregistered schema");
            }
        }
    }
    if (actions_)
    {
        for (const ExactDependencyIdentity& action : manifest.actions)
        {
            const ActionDescriptor* descriptor =
                actions_->ResolveAction(action);
            if (!descriptor ||
                descriptor->providing_pack != manifest.identity)
            {
                return Failure(
                    RegistryErrorCode::ReferenceMismatch,
                    "Capability pack action registration does not match its provider");
            }
        }
        for (const ExactDependencyIdentity& reducer : manifest.reducers)
        {
            const ReducerDescriptor* descriptor =
                actions_->ResolveReducer(reducer);
            if (!descriptor ||
                descriptor->providing_pack != manifest.identity)
            {
                return Failure(
                    RegistryErrorCode::ReferenceMismatch,
                    "Capability pack reducer registration does not match its provider");
            }
        }
    }

    return RegistryResult::Success();
}

RegistryResult CapabilityPackRegistry::ValidateCandidate(
    const ManifestMap& manifests) const
{
    enum class Visit : std::uint8_t
    {
        Visiting,
        Complete,
    };
    std::map<Key, Visit> visits;
    std::function<RegistryResult(const CapabilityPackManifest&)> visit =
        [&](const CapabilityPackManifest& manifest) -> RegistryResult {
        const Key key = MakeKey(manifest.identity);
        const auto prior = visits.find(key);
        if (prior != visits.end())
        {
            if (prior->second == Visit::Visiting)
            {
                return Failure(
                    RegistryErrorCode::DependencyCycle,
                    "Capability pack dependency cycle: " +
                        manifest.identity.canonical_id);
            }
            return RegistryResult::Success();
        }

        visits.emplace(key, Visit::Visiting);
        for (const CapabilityPackIdentity& dependency :
             manifest.dependencies)
        {
            const auto found = manifests.find(MakeKey(dependency));
            if (found == manifests.end())
            {
                return Failure(
                    RegistryErrorCode::DependencyMissing,
                    "Capability pack dependency is not registered: " +
                        dependency.canonical_id);
            }
            if (found->second.identity != dependency)
            {
                return Failure(
                    RegistryErrorCode::ReferenceMismatch,
                    "Capability pack dependency hash mismatch: " +
                        dependency.canonical_id);
            }
            if (found->second.compatibility != manifest.compatibility)
            {
                return Failure(
                    RegistryErrorCode::CompatibilityMismatch,
                    "Capability pack dependencies require identical compatibility");
            }
            const RegistryResult nested = visit(found->second);
            if (!nested.success)
                return nested;
        }
        visits[key] = Visit::Complete;
        return RegistryResult::Success();
    };

    for (const auto& [key, manifest] : manifests)
    {
        (void)key;
        const RegistryResult result = visit(manifest);
        if (!result.success)
            return result;
    }
    return RegistryResult::Success();
}

} // namespace savor::runtime::program
