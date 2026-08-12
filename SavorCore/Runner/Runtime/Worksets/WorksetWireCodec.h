#pragma once

#include "WorksetTypes.h"

#include <span>
#include <string>
#include <vector>

namespace savor::runtime {

inline constexpr std::uint32_t kWorksetWireVersionV4 = 4;
inline constexpr std::uint32_t kWorkerRuntimeContractWireVersionV1 = 1;
inline constexpr std::uint32_t kWorksetCaptureBindingWireVersionV1 = 1;
inline constexpr std::uint32_t kWorksetDerivedStateBindingWireVersionV1 = 1;
inline constexpr std::uint32_t kProgressPlanWireVersionV1 = 1;
inline constexpr std::size_t kMaximumWorksetWireBytes =
    32ull * 1024ull * 1024ull;

struct WorksetWireCodecResult
{
    bool ok = false;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept { return ok; }
};

[[nodiscard]] WorksetWireCodecResult EncodeWorkerWorksetV4(
    const WorkerWorksetDefinition& definition,
    std::vector<std::uint8_t>& output);

[[nodiscard]] WorksetWireCodecResult DecodeWorkerWorksetV4(
    std::span<const std::uint8_t> input,
    WorkerWorksetDefinition& output);

[[nodiscard]] WorksetWireCodecResult EncodeWorksetCaptureBindingV1(
    const std::optional<WorksetCaptureBindingV1>& binding,
    std::vector<std::uint8_t>& output);

[[nodiscard]] WorksetWireCodecResult DecodeWorksetCaptureBindingV1(
    std::span<const std::uint8_t> input,
    std::optional<WorksetCaptureBindingV1>& output);

[[nodiscard]] WorksetWireCodecResult EncodeWorksetDerivedStateBindingV1(
    const derived::WorksetDerivedStateBindingV1& binding,
    std::vector<std::uint8_t>& output);

[[nodiscard]] WorksetWireCodecResult DecodeWorksetDerivedStateBindingV1(
    std::span<const std::uint8_t> input,
    derived::WorksetDerivedStateBindingV1& output);

[[nodiscard]] WorksetWireCodecResult EncodeProgressPlanV1(
    const progress::ProgressPlanV1& plan,
    std::vector<std::uint8_t>& output);

[[nodiscard]] WorksetWireCodecResult DecodeProgressPlanV1(
    std::span<const std::uint8_t> input,
    progress::ProgressPlanV1& output);

[[nodiscard]] WorksetWireCodecResult EncodeWorkerRuntimeContractV1(
    const WorkerRuntimeContractV1& contract,
    std::vector<std::uint8_t>& output);

[[nodiscard]] WorksetWireCodecResult DecodeWorkerRuntimeContractV1(
    std::span<const std::uint8_t> input,
    WorkerRuntimeContractV1& output);

} // namespace savor::runtime
