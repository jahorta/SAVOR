#pragma once

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Sa3Dport::Structs {

template <class T>
struct LabeledArray {
    std::string label;
    std::vector<T> values;

    LabeledArray() = default;
    explicit LabeledArray(std::vector<T> values_) : values(std::move(values_)) {}
    LabeledArray(std::string label_, std::vector<T> values_)
        : label(std::move(label_)), values(std::move(values_)) {}
};

template <class T>
struct LabeledReadOnlyArray {
    std::string label;
    std::vector<T> values;

    LabeledReadOnlyArray() = default;
    explicit LabeledReadOnlyArray(std::vector<T> values_) : values(std::move(values_)) {}
    LabeledReadOnlyArray(std::string label_, std::vector<T> values_)
        : label(std::move(label_)), values(std::move(values_)) {}

    [[nodiscard]] std::span<const T> span() const {
        return values;
    }
};

} // namespace Sa3Dport::Structs
