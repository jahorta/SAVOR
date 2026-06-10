#pragma once

#include <algorithm>

namespace savorqt::gui {

template <typename Container, typename Equal>
bool RowsEqual(const Container& lhs, const Container& rhs, Equal equal)
{
    return lhs.size() == rhs.size()
        && std::equal(lhs.begin(), lhs.end(), rhs.begin(), equal);
}

template <typename Value>
bool AssignIfChanged(Value& target, const Value& value)
{
    if (target == value) {
        return false;
    }
    target = value;
    return true;
}

} // namespace savorqt::gui
