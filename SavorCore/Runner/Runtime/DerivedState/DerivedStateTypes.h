#pragma once

#include "../RuntimeTypes.h"
#include "../StopPoints/StopPointTypes.h"

#include <compare>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace savor::runtime::derived {

inline constexpr std::uint32_t kWorksetDerivedStateBindingVersionV1 = 1;

enum class DerivedStateFreshness : std::uint8_t
{
    SameRoutedEvent = 1,
    LatestInItem = 2,
};

struct DerivedStateBlockIdentityV1
{
    std::string canonical_id;
    std::uint32_t revision = 0;
    std::string descriptor_sha256;

    [[nodiscard]] explicit operator bool() const noexcept;
    auto operator<=>(const DerivedStateBlockIdentityV1&) const = default;
};

struct DerivedStateBlockBindingV1
{
    DerivedStateBlockIdentityV1 identity;
    std::vector<std::uint8_t> configuration;
    std::string configuration_sha256;
    std::string content_sha256;

    [[nodiscard]] explicit operator bool() const noexcept;
    auto operator<=>(const DerivedStateBlockBindingV1&) const = default;
};

struct WorksetDerivedStateBindingV1
{
    WorksetDerivedStateBindingV1();

    std::uint32_t version = kWorksetDerivedStateBindingVersionV1;
    std::vector<DerivedStateBlockBindingV1> blocks;
    std::string content_sha256;

    [[nodiscard]] explicit operator bool() const noexcept;
    auto operator<=>(const WorksetDerivedStateBindingV1&) const = default;
};

struct DerivedStateSnapshotProvenanceV1
{
    WorksetEpoch workset_epoch;
    WorkerWorksetItemId item_id;
    DerivedStateBlockIdentityV1 block;
    std::string group_id;
    std::uint32_t group_revision = 0;
    std::uint64_t generation = 0;
    RoutedStopIdentity routed_stop;
    std::uint32_t trigger_pc = 0;

    [[nodiscard]] explicit operator bool() const noexcept;
    auto operator<=>(const DerivedStateSnapshotProvenanceV1&) const = default;
};

[[nodiscard]] std::string ComputeDerivedStateBlockBindingHashV1(
    const DerivedStateBlockBindingV1& binding);

[[nodiscard]] std::string ComputeWorksetDerivedStateBindingHashV1(
    const WorksetDerivedStateBindingV1& binding);

[[nodiscard]] bool ValidateWorksetDerivedStateBindingV1(
    const WorksetDerivedStateBindingV1& binding,
    std::string* error_out = nullptr);

} // namespace savor::runtime::derived
