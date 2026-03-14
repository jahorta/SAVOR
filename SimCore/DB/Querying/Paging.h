#pragma once
#include <cstdint>
#include <optional>
#include <vector>

struct KeysetCursor {
    int64_t primary{};
    int64_t secondary{};
};

template <typename T>
struct Page {
    std::vector<T> items;
    std::optional<KeysetCursor> next;
    std::optional<KeysetCursor> prev;
};
