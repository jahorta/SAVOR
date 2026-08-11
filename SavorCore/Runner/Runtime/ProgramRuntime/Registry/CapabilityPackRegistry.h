#pragma once

#include "ActionRegistry.h"
#include "RegistryResult.h"
#include "TypeSchemaRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Model/ProgramIdentifiers.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace savor::runtime::program {

struct RuntimeCompatibility
{
    std::string game_id;
    std::string executable_identity;
    std::string address_map_revision;

    auto operator<=>(const RuntimeCompatibility&) const = default;
};

struct SemanticPointDescriptor
{
    std::string canonical_id;
    SemanticPointKind kind = SemanticPointKind::ProgramCounter;
    std::uint32_t pc = 0;
    std::uint32_t memory_address = 0;
    std::uint32_t memory_size = 0;
    bool memory_read = false;
    bool memory_write = false;
    std::string synthetic_identity;
    auto operator<=>(const SemanticPointDescriptor&) const = default;
};

struct AddressSymbolDescriptor
{
    std::string canonical_id;
    std::uint32_t address = 0;
    TypeRef value_type;

    auto operator<=>(const AddressSymbolDescriptor&) const = default;
};

struct CoherentQueryDescriptor
{
    std::string canonical_id;
    SchemaIdentity result_schema;
    std::vector<std::string> address_dependencies;
    std::uint64_t maximum_guest_reads = 0;

    auto operator<=>(const CoherentQueryDescriptor&) const = default;
};

enum class CpuEvaluatorOperation : std::uint8_t
{
    ReadU8,
    ReadU16,
    ReadU32,
    ReadU64,
    CompareEqual,
    CompareMaskedEqual,
};

struct CpuEvaluatorDescriptor
{
    std::string canonical_id;
    // Stable router-local sample identity carried by routed stop evidence.
    // This is part of the homogeneous runtime ABI and never supplied by a
    // workset.
    std::uint32_t routed_sample_descriptor_id = 0;
    // Empty only for evaluators whose value comes from the native hit itself.
    // Guest-memory evaluators name an address symbol in this manifest.
    std::string address_dependency;
    // Exact IR type produced by this static evaluator. SPS1 requirements must
    // link to this type during program verification.
    TypeRef result_type;
    std::vector<CpuEvaluatorOperation> operations;
    std::uint32_t maximum_reads = 0;
    std::uint32_t maximum_output_bytes = 0;

    auto operator<=>(const CpuEvaluatorDescriptor&) const = default;
};

struct CapabilityPackManifest
{
    CapabilityPackIdentity identity;
    RuntimeCompatibility compatibility;
    std::vector<CapabilityPackIdentity> dependencies;
    std::vector<SchemaIdentity> schemas;
    std::vector<SemanticPointDescriptor> semantic_points;
    std::vector<AddressSymbolDescriptor> address_symbols;
    std::vector<CoherentQueryDescriptor> coherent_queries;
    std::vector<CpuEvaluatorDescriptor> cpu_evaluators;
    std::vector<ExactDependencyIdentity> actions;
    std::vector<ExactDependencyIdentity> reducers;

    auto operator<=>(const CapabilityPackManifest&) const = default;
};

struct CapabilityPackClosure
{
    std::vector<CapabilityPackManifest> dependency_order;
};

// Hashes the complete normalized manifest. Inventory collections are treated
// as sets; semantically ordered collections inside an entry (such as CPU
// evaluator operations and reducer input types, represented by their exact
// dependency identities) retain their order.
[[nodiscard]] ContentHash256 ComputeCapabilityPackManifestContractHash(
    const CapabilityPackManifest& manifest);

class CapabilityPackRegistry final
{
public:
    CapabilityPackRegistry(
        const TypeSchemaRegistry* schemas = nullptr,
        const ActionRegistry* actions = nullptr) noexcept
        : schemas_(schemas), actions_(actions)
    {
    }

    [[nodiscard]] RegistryResult Register(
        CapabilityPackManifest manifest);
    [[nodiscard]] RegistryResult RegisterBatch(
        std::vector<CapabilityPackManifest> manifests);

    [[nodiscard]] const CapabilityPackManifest* Resolve(
        const CapabilityPackIdentity& identity) const noexcept;
    [[nodiscard]] std::optional<CapabilityPackClosure> ResolveClosure(
        const std::vector<CapabilityPackIdentity>& roots,
        const RuntimeCompatibility& compatibility,
        RegistryError* error = nullptr) const;

    [[nodiscard]] std::size_t size() const noexcept
    {
        return manifests_.size();
    }

private:
    using Key = std::pair<std::string, std::uint32_t>;
    using ManifestMap = std::map<Key, CapabilityPackManifest>;

    [[nodiscard]] RegistryResult ValidateManifestShape(
        const CapabilityPackManifest& manifest) const;
    [[nodiscard]] RegistryResult ValidateCandidate(
        const ManifestMap& manifests) const;

    const TypeSchemaRegistry* schemas_ = nullptr;
    const ActionRegistry* actions_ = nullptr;
    ManifestMap manifests_;
};

} // namespace savor::runtime::program
