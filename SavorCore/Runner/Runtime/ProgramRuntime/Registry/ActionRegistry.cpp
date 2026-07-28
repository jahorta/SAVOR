#include "ActionRegistry.h"

#include "Utils/Hash.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace savor::runtime::program {
namespace {

using Key = std::pair<std::string, std::uint32_t>;

Key MakeKey(const ExactDependencyIdentity& identity)
{
    return {identity.canonical_id, identity.version};
}

bool Valid(const ExactDependencyIdentity& identity)
{
    return !identity.canonical_id.empty() &&
        identity.version != 0 &&
        !identity.signature_hash.empty();
}

bool Valid(const CapabilityPackIdentity& identity)
{
    return !identity.canonical_id.empty() &&
        identity.version != 0 &&
        !identity.manifest_hash.empty();
}

RegistryResult IdentityConflict(const std::string& id)
{
    return RegistryResult::Failure(
        RegistryErrorCode::IdentityConflict,
        "Identity already has different registered content: " + id);
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

void AppendOptionalType(
    std::string& contract,
    const std::optional<TypeRef>& type)
{
    AppendNumber(contract, type.has_value());
    if (type)
        AppendType(contract, *type);
}

void AppendDependency(
    std::string& contract,
    const ExactDependencyIdentity& identity)
{
    AppendString(contract, identity.canonical_id);
    AppendNumber(contract, identity.version);
    AppendHash(contract, identity.signature_hash);
}

ContentHash256 ContractHash(std::string_view contract)
{
    const std::string digest =
        hash::sha256(contract.data(), contract.size());
    const auto result = ContentHash256::FromHex(digest);
    if (!result)
        throw std::logic_error("Registry contract hash failed");
    return *result;
}

} // namespace

ContentHash256 ComputeActionDescriptorContractHash(
    const ActionDescriptor& descriptor)
{
    std::string contract("action-descriptor/1;");
    AppendString(contract, descriptor.identity.canonical_id);
    AppendNumber(contract, descriptor.identity.version);
    AppendString(contract, descriptor.providing_pack.canonical_id);
    AppendNumber(contract, descriptor.providing_pack.version);
    AppendType(contract, descriptor.input_type);
    AppendType(contract, descriptor.output_type);
    AppendOptionalType(contract, descriptor.domain_observation_type);
    AppendOptionalType(contract, descriptor.receipt_type);
    AppendOptionalType(contract, descriptor.diagnostic_type);
    AppendNumber(contract, descriptor.required_services);
    AppendNumber(contract, descriptor.effects);
    AppendNumber(contract, descriptor.epoch_policy);
    AppendNumber(contract, descriptor.replay_class);
    AppendNumber(contract, descriptor.cancellation);
    AppendNumber(
        contract,
        descriptor.maximum_non_cancellable_milliseconds);
    AppendNumber(
        contract,
        descriptor.default_deadline_milliseconds);
    AppendNumber(contract, descriptor.resource_behavior);
    AppendNumber(contract, descriptor.cleanup);
    AppendNumber(contract, descriptor.taints_on_unproven_cleanup);
    AppendNumber(contract, descriptor.idempotency);

    auto categories = descriptor.diagnostic_categories;
    std::ranges::sort(categories);
    AppendNumber(contract, categories.size());
    for (const std::string& category : categories)
        AppendString(contract, category);
    return ContractHash(contract);
}

ContentHash256 ComputeReducerDescriptorContractHash(
    const ReducerDescriptor& descriptor)
{
    std::string contract("reducer-descriptor/1;");
    AppendString(contract, descriptor.identity.canonical_id);
    AppendNumber(contract, descriptor.identity.version);
    AppendString(contract, descriptor.providing_pack.canonical_id);
    AppendNumber(contract, descriptor.providing_pack.version);
    AppendNumber(contract, descriptor.input_types.size());
    for (const TypeRef& input : descriptor.input_types)
        AppendType(contract, input);
    AppendType(contract, descriptor.output_type);

    auto actions = descriptor.permitted_actions;
    std::ranges::sort(actions);
    AppendNumber(contract, actions.size());
    for (const ExactDependencyIdentity& action : actions)
        AppendDependency(contract, action);

    auto subprograms = descriptor.permitted_subprograms;
    std::ranges::sort(subprograms);
    AppendNumber(contract, subprograms.size());
    for (const ExactDependencyIdentity& subprogram : subprograms)
        AppendDependency(contract, subprogram);

    AppendNumber(contract, descriptor.maximum_steps);
    AppendNumber(contract, descriptor.maximum_value_bytes);
    return ContractHash(contract);
}

RegistryResult ActionRegistry::RegisterAction(ActionDescriptor descriptor)
{
    std::vector<ActionDescriptor> actions;
    actions.push_back(std::move(descriptor));
    return RegisterCatalog(std::move(actions), {});
}

RegistryResult ActionRegistry::RegisterReducer(ReducerDescriptor descriptor)
{
    std::vector<ReducerDescriptor> reducers;
    reducers.push_back(std::move(descriptor));
    return RegisterCatalog({}, std::move(reducers));
}

RegistryResult ActionRegistry::RegisterCatalog(
    std::vector<ActionDescriptor> actions,
    std::vector<ReducerDescriptor> reducers)
{
    auto candidate_actions = actions_;
    auto candidate_reducers = reducers_;

    for (ActionDescriptor& descriptor : actions)
    {
        const RegistryResult validation = Validate(descriptor);
        if (!validation.success)
            return validation;

        const Key key = MakeKey(descriptor.identity);
        const auto existing = candidate_actions.find(key);
        if (existing != candidate_actions.end())
        {
            if (existing->second == descriptor)
                continue;
            return IdentityConflict(descriptor.identity.canonical_id);
        }
        candidate_actions.emplace(key, std::move(descriptor));
    }

    for (ReducerDescriptor& descriptor : reducers)
    {
        const RegistryResult validation = Validate(descriptor);
        if (!validation.success)
            return validation;

        const Key key = MakeKey(descriptor.identity);
        const auto existing = candidate_reducers.find(key);
        if (existing != candidate_reducers.end())
        {
            if (existing->second == descriptor)
                continue;
            return IdentityConflict(descriptor.identity.canonical_id);
        }
        candidate_reducers.emplace(key, std::move(descriptor));
    }

    for (const auto& [key, descriptor] : candidate_reducers)
    {
        (void)key;
        for (const ExactDependencyIdentity& action :
             descriptor.permitted_actions)
        {
            const auto found = candidate_actions.find(MakeKey(action));
            if (found == candidate_actions.end() ||
                found->second.identity.signature_hash !=
                    action.signature_hash)
            {
                return RegistryResult::Failure(
                    RegistryErrorCode::DependencyMissing,
                    "Reducer permits an unregistered action: " +
                        action.canonical_id);
            }
        }
    }

    actions_ = std::move(candidate_actions);
    reducers_ = std::move(candidate_reducers);
    return RegistryResult::Success();
}

const ActionDescriptor* ActionRegistry::ResolveAction(
    const ExactDependencyIdentity& identity) const noexcept
{
    const auto found = actions_.find(MakeKey(identity));
    if (found == actions_.end() ||
        found->second.identity.signature_hash != identity.signature_hash)
    {
        return nullptr;
    }
    return &found->second;
}

const ReducerDescriptor* ActionRegistry::ResolveReducer(
    const ExactDependencyIdentity& identity) const noexcept
{
    const auto found = reducers_.find(MakeKey(identity));
    if (found == reducers_.end() ||
        found->second.identity.signature_hash != identity.signature_hash)
    {
        return nullptr;
    }
    return &found->second;
}

RegistryResult ActionRegistry::Validate(
    const ActionDescriptor& descriptor) const
{
    if (!Valid(descriptor.identity) || !Valid(descriptor.providing_pack))
    {
        return RegistryResult::Failure(
            RegistryErrorCode::InvalidIdentity,
            "Action requires exact identity and providing pack");
    }
    if (!IsKnown(descriptor.input_type) ||
        !IsKnown(descriptor.output_type) ||
        (descriptor.domain_observation_type &&
         !IsKnown(*descriptor.domain_observation_type)) ||
        (descriptor.receipt_type && !IsKnown(*descriptor.receipt_type)) ||
        (descriptor.diagnostic_type &&
         !IsKnown(*descriptor.diagnostic_type)))
    {
        return RegistryResult::Failure(
            RegistryErrorCode::DependencyMissing,
            "Action references an unregistered schema: " +
                descriptor.identity.canonical_id);
    }
    if (descriptor.default_deadline_milliseconds == 0)
    {
        return RegistryResult::Failure(
            RegistryErrorCode::InvalidArgument,
            "Actions require a finite nonzero default deadline");
    }
    if (descriptor.resource_behavior != ActionResourceBehavior::None &&
        descriptor.cleanup == ActionCleanupGuarantee::None)
    {
        return RegistryResult::Failure(
            RegistryErrorCode::InvalidArgument,
            "Resource-producing actions require cleanup behavior");
    }
    if (descriptor.taints_on_unproven_cleanup &&
        descriptor.cleanup == ActionCleanupGuarantee::None)
    {
        return RegistryResult::Failure(
            RegistryErrorCode::InvalidArgument,
            "Cleanup taint policy requires a cleanup guarantee");
    }
    std::set<std::string> categories;
    for (const std::string& category : descriptor.diagnostic_categories)
    {
        if (category.empty() || !categories.emplace(category).second)
        {
            return RegistryResult::Failure(
                RegistryErrorCode::InvalidArgument,
                "Action diagnostic categories must be unique and nonempty");
        }
    }
    return RegistryResult::Success();
}

RegistryResult ActionRegistry::Validate(
    const ReducerDescriptor& descriptor) const
{
    if (!Valid(descriptor.identity) || !Valid(descriptor.providing_pack))
    {
        return RegistryResult::Failure(
            RegistryErrorCode::InvalidIdentity,
            "Reducer requires exact identity and providing pack");
    }
    if (descriptor.maximum_steps == 0 ||
        descriptor.maximum_value_bytes == 0)
    {
        return RegistryResult::Failure(
            RegistryErrorCode::InvalidArgument,
            "Reducer requires finite computation and allocation budgets");
    }
    if (!IsKnown(descriptor.output_type))
    {
        return RegistryResult::Failure(
            RegistryErrorCode::DependencyMissing,
            "Reducer output schema is not registered");
    }
    for (const TypeRef& input : descriptor.input_types)
    {
        if (!IsKnown(input))
        {
            return RegistryResult::Failure(
                RegistryErrorCode::DependencyMissing,
                "Reducer input schema is not registered");
        }
    }
    std::set<ExactDependencyIdentity> selections;
    for (const ExactDependencyIdentity& action :
         descriptor.permitted_actions)
    {
        if (!Valid(action) || !selections.emplace(action).second)
        {
            return RegistryResult::Failure(
                RegistryErrorCode::InvalidArgument,
                "Reducer permitted actions must be exact and unique");
        }
    }
    for (const ExactDependencyIdentity& subprogram :
         descriptor.permitted_subprograms)
    {
        if (!Valid(subprogram))
        {
            return RegistryResult::Failure(
                RegistryErrorCode::InvalidIdentity,
                "Reducer permitted subprogram identity is invalid");
        }
    }
    return RegistryResult::Success();
}

bool ActionRegistry::IsKnown(const TypeRef& type) const noexcept
{
    if (!type.is_named())
        return true;
    return schemas_ == nullptr || schemas_->Resolve(*type.named) != nullptr;
}

} // namespace savor::runtime::program
