#pragma once

#include "WorksetTypes.h"

#include <span>
#include <string>
#include <vector>

namespace savor::runtime {

inline constexpr std::uint32_t kWorksetWireVersionV1 = 1;
inline constexpr std::size_t kMaximumWorksetWireBytes =
    32ull * 1024ull * 1024ull;

struct WorksetWireCodecResult
{
    bool ok = false;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept { return ok; }
};

[[nodiscard]] WorksetWireCodecResult EncodeWorkerWorksetV1(
    const WorkerWorksetDefinition& definition,
    std::vector<std::uint8_t>& output);

[[nodiscard]] WorksetWireCodecResult DecodeWorkerWorksetV1(
    std::span<const std::uint8_t> input,
    WorkerWorksetDefinition& output);

[[nodiscard]] WorksetWireCodecResult EncodeWorkerRuntimeManifestV1(
    const WorkerRuntimeManifest& manifest,
    std::vector<std::uint8_t>& output);

[[nodiscard]] WorksetWireCodecResult DecodeWorkerRuntimeManifestV1(
    std::span<const std::uint8_t> input,
    WorkerRuntimeManifest& output);

} // namespace savor::runtime
