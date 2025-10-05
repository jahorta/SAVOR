#pragma once
#include "Paging.h"
#include <optional>

enum class PageOrder { Desc, Asc };

template <typename TCursor = KeysetCursor>
struct PagedQuery {
    PageOrder order{ PageOrder::Desc };
    std::optional<TCursor> before;
    std::optional<TCursor> after;
    int limit{ 50 };
};
