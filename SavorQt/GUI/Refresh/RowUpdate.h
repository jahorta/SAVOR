#pragma once

#include "GUI/Widgets/ScrollBarStabilizer.h"

#include <QtCore/QSignalBlocker>
#include <QtCore/QVariant>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>

#include <algorithm>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace savorqt::gui {

struct KeyedWidgetItem {
    QVariant key;
    QString text;
    QString tooltip;
    bool enabled = true;
};

enum class MissingSelectionPolicy {
    Clear,
    PreserveUnresolved,
    SelectFirstOnInitialLoad,
};

inline void ReconcileComboItems(QComboBox* combo, const std::vector<KeyedWidgetItem>& items,
                                const QVariant& selected_key, MissingSelectionPolicy policy,
                                bool initial_hydration = false)
{
    if (combo == nullptr) {
        return;
    }
    const QSignalBlocker blocker(combo);
    combo->clear();
    int selected_index = -1;
    for (const auto& item : items) {
        combo->addItem(item.text, item.key);
        const int index = combo->count() - 1;
        combo->setItemData(index, item.tooltip, Qt::ToolTipRole);
        if (auto* model = qobject_cast<QStandardItemModel*>(combo->model())) {
            if (auto* standard_item = model->item(index)) {
                standard_item->setEnabled(item.enabled);
            }
        }
        if (item.key == selected_key) {
            selected_index = index;
        }
    }
    if (selected_index < 0 && selected_key.isValid()
        && policy == MissingSelectionPolicy::PreserveUnresolved) {
        combo->addItem(QStringLiteral("Unavailable (%1)").arg(selected_key.toString()), selected_key);
        selected_index = combo->count() - 1;
        if (auto* model = qobject_cast<QStandardItemModel*>(combo->model())) {
            if (auto* standard_item = model->item(selected_index)) {
                standard_item->setEnabled(false);
            }
        }
    }
    if (selected_index < 0 && initial_hydration
        && policy == MissingSelectionPolicy::SelectFirstOnInitialLoad && !items.empty()) {
        selected_index = 0;
    }
    combo->setCurrentIndex(selected_index);
}

inline void ReconcileListItems(QListWidget* list, const std::vector<KeyedWidgetItem>& items,
                               const QVariant& selected_key, MissingSelectionPolicy policy,
                               bool initial_hydration = false)
{
    if (list == nullptr) {
        return;
    }
    const int scroll_value = list->verticalScrollBar()->value();
    const QSignalBlocker blocker(list);
    list->clear();
    QListWidgetItem* selected_item = nullptr;
    for (const auto& item : items) {
        auto* row = new QListWidgetItem(item.text, list);
        row->setData(Qt::UserRole, item.key);
        row->setToolTip(item.tooltip);
        if (!item.enabled) {
            row->setFlags(row->flags() & ~Qt::ItemIsEnabled);
        }
        if (item.key == selected_key) {
            selected_item = row;
        }
    }
    if (selected_item == nullptr && selected_key.isValid()
        && policy == MissingSelectionPolicy::PreserveUnresolved) {
        selected_item = new QListWidgetItem(
            QStringLiteral("Unavailable (%1)").arg(selected_key.toString()), list);
        selected_item->setData(Qt::UserRole, selected_key);
        selected_item->setFlags(selected_item->flags() & ~Qt::ItemIsEnabled);
    }
    if (selected_item == nullptr && initial_hydration
        && policy == MissingSelectionPolicy::SelectFirstOnInitialLoad && list->count() > 0) {
        selected_item = list->item(0);
    }
    list->setCurrentItem(selected_item);
    list->verticalScrollBar()->setValue(scroll_value);
}

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

    const ItemViewScrollSnapshot scrollSnapshot = captureItemViewScrollSnapshot(table);
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
    restoreItemViewScrollSnapshot(table, scrollSnapshot);
    return true;
}

template <typename Row, typename KeyFn, typename EqualFn, typename PopulateFn>
bool ApplyTreeRowsByKey(
    QTreeWidget* tree,
    std::vector<Row>& currentRows,
    const std::vector<Row>& newRows,
    KeyFn keyFn,
    EqualFn equalFn,
    PopulateFn populateFn)
{
    if (tree == nullptr) {
        currentRows = newRows;
        return true;
    }

    if (RowsEqual(currentRows, newRows, equalFn)) {
        return false;
    }

    using Key = std::decay_t<decltype(keyFn(std::declval<const Row&>()))>;

    std::optional<Key> selectedKey;
    QTreeWidgetItem* currentItem = tree->currentItem();
    while (currentItem != nullptr && currentItem->parent() != nullptr) {
        currentItem = currentItem->parent();
    }
    const int selectedRow = currentItem != nullptr ? tree->indexOfTopLevelItem(currentItem) : -1;
    if (selectedRow >= 0 && selectedRow < static_cast<int>(currentRows.size())) {
        selectedKey = keyFn(currentRows[static_cast<std::size_t>(selectedRow)]);
    }

    const ItemViewScrollSnapshot scrollSnapshot = captureItemViewScrollSnapshot(tree);
    const QSignalBlocker blocker(tree);

    for (int row = static_cast<int>(currentRows.size()) - 1; row >= 0; --row) {
        const auto key = keyFn(currentRows[static_cast<std::size_t>(row)]);
        if (!FindRowIndexByKey(newRows, keyFn, key).has_value()) {
            delete tree->takeTopLevelItem(row);
            currentRows.erase(currentRows.begin() + row);
        }
    }

    for (int row = 0; row < static_cast<int>(newRows.size()); ++row) {
        const Row& newRow = newRows[static_cast<std::size_t>(row)];
        const auto newKey = keyFn(newRow);

        if (row < static_cast<int>(currentRows.size()) && keyFn(currentRows[static_cast<std::size_t>(row)]) == newKey) {
            if (!equalFn(currentRows[static_cast<std::size_t>(row)], newRow)) {
                populateFn(tree, tree->topLevelItem(row), newRow);
                currentRows[static_cast<std::size_t>(row)] = newRow;
            }
            continue;
        }

        const auto existing = FindRowIndexByKey(currentRows, keyFn, newKey);
        if (existing.has_value()) {
            QTreeWidgetItem* item = tree->takeTopLevelItem(*existing);
            Row movedRow = currentRows[static_cast<std::size_t>(*existing)];
            currentRows.erase(currentRows.begin() + *existing);
            tree->insertTopLevelItem(row, item);
            populateFn(tree, item, newRow);
            currentRows.insert(currentRows.begin() + row, newRow);
            (void)movedRow;
        } else {
            auto* item = new QTreeWidgetItem();
            tree->insertTopLevelItem(row, item);
            populateFn(tree, item, newRow);
            currentRows.insert(currentRows.begin() + row, newRow);
        }
    }

    while (static_cast<int>(currentRows.size()) > static_cast<int>(newRows.size())) {
        const int row = static_cast<int>(currentRows.size()) - 1;
        delete tree->takeTopLevelItem(row);
        currentRows.pop_back();
    }

    if (selectedKey.has_value()) {
        const auto selected = FindRowIndexByKey(currentRows, keyFn, *selectedKey);
        if (selected.has_value()) {
            tree->setCurrentItem(tree->topLevelItem(*selected));
        }
    }
    restoreItemViewScrollSnapshot(tree, scrollSnapshot);
    return true;
}

} // namespace savorqt::gui
