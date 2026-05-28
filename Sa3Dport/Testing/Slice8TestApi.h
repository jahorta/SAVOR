#pragma once

#include "File/AnimationFile.h"

#include <cstdint>
#include <span>

namespace Sa3Dport::Testing::Slice8 {

using AnimationFile = ::Sa3Dport::File::AnimationFile;

[[nodiscard]] inline bool CheckIsAnimationFile(std::span<const std::byte> data) {
    return ::Sa3Dport::File::AnimationFile::check_is_animation_file(data);
}

[[nodiscard]] inline AnimationFile ReadAnimationFile(std::span<const std::byte> data,
                                                     std::uint32_t nodeCount,
                                                     bool shortRot = false) {
    return ::Sa3Dport::File::AnimationFile::read_from_bytes(data, nodeCount, shortRot);
}

} // namespace Sa3Dport::Testing::Slice8
