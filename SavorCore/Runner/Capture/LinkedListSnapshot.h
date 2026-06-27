#pragma once

#include "CaptureJsonlWriter.h"
#include "CaptureProfile.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace savor::capture {

using LinkedListMemoryReader =
    std::function<bool(std::uint32_t address, SampleWidth width, std::uint64_t& out)>;

std::vector<CaptureField> BuildLinkedListSnapshotFields(
    const LinkedListSnapshotSpec& spec,
    const LinkedListMemoryReader& read_memory);

} // namespace savor::capture
