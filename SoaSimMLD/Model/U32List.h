#pragma once

#include "../common/ByteUtils.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace soasim::mld::model {

struct U32List {
    std::uint32_t pointer = 0;
    bool valid = false;
    std::vector<std::uint32_t> values{};
};

using U32ListWarningSink = std::function<void(const std::string&)>;

[[nodiscard]] inline std::vector<std::uint32_t> parseU32List(std::span<const std::uint8_t> bytes,
    const std::uint32_t pointer,
    const std::string& label,
    const U32ListWarningSink& warningSink) {
    std::vector<std::uint32_t> out{};
    const std::size_t offset = static_cast<std::size_t>(pointer);
    const auto countOpt = common::readU32AtBE(bytes, offset);
    if (!countOpt.has_value()) {
        warningSink(label + " pointer out of bounds: " + std::to_string(pointer));
        return out;
    }

    const std::size_t count = static_cast<std::size_t>(*countOpt);
    constexpr std::size_t hardCap = 1U << 16;
    if (count > hardCap) {
        warningSink(label + " list count suspiciously large (" + std::to_string(count) + "); ignoring list.");
        return out;
    }

    if (offset + 4 + (count * 4) > bytes.size()) {
        warningSink(label + " list overruns file bounds (ptr=" + std::to_string(pointer) +
            ", count=" + std::to_string(count) + ")");
        return out;
    }

    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto value = common::readU32AtBE(bytes, offset + 4 + (i * 4));
        if (!value.has_value()) {
            break;
        }
        out.push_back(*value);
    }

    return out;
}

[[nodiscard]] inline std::unique_ptr<U32List> makeU32List(std::span<const std::uint8_t> bytes,
    const std::uint32_t pointer,
    const std::string& label,
    const U32ListWarningSink& warningSink) {
    auto list = std::make_unique<U32List>();
    list->pointer = pointer;
    list->values = parseU32List(bytes, pointer, label, warningSink);

    const auto countValue = common::readU32AtBE(bytes, static_cast<std::size_t>(pointer));
    list->valid = !list->values.empty() || pointer == 0 || (countValue.has_value() && countValue.value_or(0U) == 0U);
    return list;
}

} // namespace soasim::mld::model
