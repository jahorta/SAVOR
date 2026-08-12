#include "DerivedStateTypes.h"

#include "Utils/Hash.h"

#include <algorithm>
#include <set>

namespace savor::runtime::derived {
namespace {

void AppendField(std::string& output, std::string_view value)
{
    output.append(std::to_string(value.size()));
    output.push_back(':');
    output.append(value);
    output.push_back('|');
}

template <typename Value>
void AppendNumber(std::string& output, Value value)
{
    AppendField(output, std::to_string(value));
}

bool CompleteSha256(std::string_view value)
{
    return value.size() == 64 && std::ranges::all_of(
        value,
        [](char ch)
        {
            return (ch >= '0' && ch <= '9') ||
                (ch >= 'a' && ch <= 'f');
        });
}

bool Fail(std::string message, std::string* error_out)
{
    if (error_out)
        *error_out = std::move(message);
    return false;
}

} // namespace

DerivedStateBlockIdentityV1::operator bool() const noexcept
{
    return !canonical_id.empty() && revision != 0 &&
        CompleteSha256(descriptor_sha256);
}

DerivedStateBlockBindingV1::operator bool() const noexcept
{
    return static_cast<bool>(identity) &&
        CompleteSha256(configuration_sha256) &&
        CompleteSha256(content_sha256);
}

WorksetDerivedStateBindingV1::WorksetDerivedStateBindingV1()
{
    content_sha256 = ComputeWorksetDerivedStateBindingHashV1(*this);
}

WorksetDerivedStateBindingV1::operator bool() const noexcept
{
    return version == kWorksetDerivedStateBindingVersionV1 &&
        CompleteSha256(content_sha256);
}

DerivedStateSnapshotProvenanceV1::operator bool() const noexcept
{
    return workset_epoch && item_id && static_cast<bool>(block) &&
        !group_id.empty() && group_revision != 0 && generation != 0 &&
        routed_stop.workset_epoch == workset_epoch && trigger_pc != 0;
}

std::string ComputeDerivedStateBlockBindingHashV1(
    const DerivedStateBlockBindingV1& binding)
{
    std::string canonical;
    AppendField(canonical, "savor.derived-state.block-binding/v1");
    AppendField(canonical, binding.identity.canonical_id);
    AppendNumber(canonical, binding.identity.revision);
    AppendField(canonical, binding.identity.descriptor_sha256);
    AppendField(canonical, binding.configuration_sha256);
    return hash::sha256(canonical.data(), canonical.size());
}

std::string ComputeWorksetDerivedStateBindingHashV1(
    const WorksetDerivedStateBindingV1& binding)
{
    std::string canonical;
    AppendField(canonical, "savor.derived-state.workset-binding/v1");
    AppendNumber(canonical, binding.version);
    AppendNumber(canonical, binding.blocks.size());
    for (const auto& block : binding.blocks)
        AppendField(canonical, block.content_sha256);
    return hash::sha256(canonical.data(), canonical.size());
}

bool ValidateWorksetDerivedStateBindingV1(
    const WorksetDerivedStateBindingV1& binding,
    std::string* error_out)
{
    if (!binding || binding.version != kWorksetDerivedStateBindingVersionV1)
        return Fail("Derived-state binding version or identity is invalid", error_out);
    if (binding.blocks.size() > 16)
        return Fail("Derived-state binding exceeds the block limit", error_out);

    std::string previous;
    for (const auto& block : binding.blocks)
    {
        if (!block)
            return Fail("Derived-state block binding is incomplete", error_out);
        if (!previous.empty() && block.identity.canonical_id <= previous)
        {
            return Fail(
                "Derived-state block bindings must be uniquely sorted by canonical ID",
                error_out);
        }
        if (hash::sha256(
                block.configuration.data(),
                block.configuration.size()) != block.configuration_sha256)
        {
            return Fail("Derived-state block configuration hash is invalid", error_out);
        }
        if (ComputeDerivedStateBlockBindingHashV1(block) != block.content_sha256)
            return Fail("Derived-state block binding hash is invalid", error_out);
        previous = block.identity.canonical_id;
    }
    if (ComputeWorksetDerivedStateBindingHashV1(binding) !=
        binding.content_sha256)
    {
        return Fail("Derived-state workset binding hash is invalid", error_out);
    }
    return true;
}

} // namespace savor::runtime::derived
