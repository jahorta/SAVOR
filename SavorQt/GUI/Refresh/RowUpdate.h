#pragma once

#include <QtCore/QSignalBlocker>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTableWidget>

#include <algorithm>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

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

template <typename Row, typename KeyFn, typename Key>
std::optional<int> FindRowIndexByKey(const std::vector<Row>& rows, KeyFn keyFn, const Key& key)
{
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        if (keyFn(rows[static_cast<std::size_t>(i)]) == key) {
            return i;
        }
    }
    return std::nullopt;
}

template <typename Row, typename KeyFn, typename EqualFn, typename PopulateFn>
bool ApplyTableRowsByKey(
    QTableWidget* table,
    std::vector<Row>& currentRows,
    const std::vector<Row>& newRows,
    KeyFn keyFn,
    EqualFn equalFn,
    PopulateFn populateFn)
{
    if (table == nullptr) {
        currentRows = newRows;
        return true;
    }

    if (RowsEqual(currentRows, newRows, equalFn)) {
        return false;
    }

    using Key = std::decay_t<decltype(keyFn(std::declval<const Row&>()))>;

    std::optional<Key> selectedKey;
    const int selectedRow = table->currentRow();
    if (selectedRow >= 0 && selectedRow < static_cast<int>(currentRows.size())) {
        selectedKey = keyFn(currentRows[static_cast<std::size_t>(selectedRow)]);
    }

    const int scrollValue = table->verticalScrollBar() == nullptr ? 0 : table->verticalScrollBar()->value();
    const QSignalBlocker blocker(table);

    for (int row = static_cast<int>(currentRows.size()) - 1; row >= 0; --row) {
        const auto key = keyFn(currentRows[static_cast<std::size_t>(row)]);
        if (!FindRowIndexByKey(newRows, keyFn, key).has_value()) {
            table->removeRow(row);
            currentRows.erase(currentRows.begin() + row);
        }
    }

    for (int row = 0; row < static_cast<int>(newRows.size()); ++row) {
        const Row& newRow = newRows[static_cast<std::size_t>(row)];
        const auto newKey = keyFn(newRow);

        if (row < static_cast<int>(currentRows.size()) && keyFn(currentRows[static_cast<std::size_t>(row)]) == newKey) {
            if (!equalFn(currentRows[static_cast<std::size_t>(row)], newRow)) {
                populateFn(table, row, newRow);
                currentRows[static_cast<std::size_t>(row)] = newRow;
            }
            continue;
        }

        const auto existing = FindRowIndexByKey(currentRows, keyFn, newKey);
        if (existing.has_value()) {
            table->removeRow(*existing);
            Row movedRow = currentRows[static_cast<std::size_t>(*existing)];
            currentRows.erase(currentRows.begin() + *existing);
            table->insertRow(row);
            populateFn(table, row, newRow);
            currentRows.insert(currentRows.begin() + row, newRow);
            (void)movedRow;
        } else {
            table->insertRow(row);
            populateFn(table, row, newRow);
            currentRows.insert(currentRows.begin() + row, newRow);
        }
    }

    while (static_cast<int>(currentRows.size()) > static_cast<int>(newRows.size())) {
        const int row = static_cast<int>(currentRows.size()) - 1;
        table->removeRow(row);
        currentRows.pop_back();
    }

    if (selectedKey.has_value()) {
        const auto selected = FindRowIndexByKey(currentRows, keyFn, *selectedKey);
        if (selected.has_value()) {
            table->selectRow(*selected);
        }
    }
    if (table->verticalScrollBar() != nullptr) {
        table->verticalScrollBar()->setValue(scrollValue);
    }
    return true;
}

} // namespace savorqt::gui
