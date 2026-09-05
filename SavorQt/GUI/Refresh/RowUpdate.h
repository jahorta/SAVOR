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

struct KeyedProjectionFailure
{
    enum class Code {
        None,
        DuplicateKey,
    };

    Code code = Code::None;
    int firstRow = -1;
    int conflictingRow = -1;
};

template <typename Row, typename KeyFn>
std::optional<KeyedProjectionFailure> ValidateProjectionKeys(const std::vector<Row>& rows, KeyFn keyFn)
{
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        const auto key = keyFn(rows[static_cast<std::size_t>(row)]);
        for (int prior = 0; prior < row; ++prior) {
            if (keyFn(rows[static_cast<std::size_t>(prior)]) == key) {
                return KeyedProjectionFailure{
                    KeyedProjectionFailure::Code::DuplicateKey,
                    prior,
                    row,
                };
            }
        }
    }
    return std::nullopt;
}

template <typename Row, typename KeyFn, typename EqualFn, typename PopulateFn>
bool ReplaceTableProjectionByKey(
    QTableWidget* table,
    std::vector<Row>& currentRows,
    const std::vector<Row>& newRows,
    KeyFn keyFn,
    EqualFn equalFn,
    PopulateFn populateFn,
    KeyedProjectionFailure* failureOut = nullptr)
{
    if (failureOut != nullptr) *failureOut = {};
    if (const auto failure = ValidateProjectionKeys(newRows, keyFn); failure.has_value()) {
        if (failureOut != nullptr) *failureOut = *failure;
        return false;
    }
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

    table->clearContents();
    table->setRowCount(static_cast<int>(newRows.size()));
    for (int row = 0; row < static_cast<int>(newRows.size()); ++row)
        populateFn(table, row, newRows[static_cast<std::size_t>(row)]);
    currentRows = newRows;

    if (selectedKey.has_value()) {
        for (int row = 0; row < static_cast<int>(currentRows.size()); ++row) {
            if (keyFn(currentRows[static_cast<std::size_t>(row)]) == *selectedKey) {
                table->selectRow(row);
                break;
            }
        }
    }
    restoreItemViewScrollSnapshot(table, scrollSnapshot);
    return true;
}

template <typename Row, typename KeyFn, typename EqualFn, typename PopulateFn>
bool ReplaceTreeProjectionByKey(
    QTreeWidget* tree,
    std::vector<Row>& currentRows,
    const std::vector<Row>& newRows,
    KeyFn keyFn,
    EqualFn equalFn,
    PopulateFn populateFn,
    KeyedProjectionFailure* failureOut = nullptr)
{
    if (failureOut != nullptr) *failureOut = {};
    if (const auto failure = ValidateProjectionKeys(newRows, keyFn); failure.has_value()) {
        if (failureOut != nullptr) *failureOut = *failure;
        return false;
    }
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

    std::vector<Key> expandedKeys;
    for (int row = 0; row < static_cast<int>(currentRows.size()) && row < tree->topLevelItemCount(); ++row) {
        const auto* item = tree->topLevelItem(row);
        if (item != nullptr && item->isExpanded()) {
            expandedKeys.push_back(keyFn(currentRows[static_cast<std::size_t>(row)]));
        }
    }

    const ItemViewScrollSnapshot scrollSnapshot = captureItemViewScrollSnapshot(tree);
    const QSignalBlocker blocker(tree);

    tree->clear();
    for (int row = 0; row < static_cast<int>(newRows.size()); ++row) {
        auto* item = new QTreeWidgetItem();
        tree->addTopLevelItem(item);
        populateFn(tree, item, newRows[static_cast<std::size_t>(row)]);
    }
    currentRows = newRows;

    for (int row = 0; row < static_cast<int>(currentRows.size()); ++row) {
        const auto key = keyFn(currentRows[static_cast<std::size_t>(row)]);
        if (std::find(expandedKeys.begin(), expandedKeys.end(), key) != expandedKeys.end()) {
            tree->topLevelItem(row)->setExpanded(true);
        }
    }

    if (selectedKey.has_value()) {
        for (int row = 0; row < static_cast<int>(currentRows.size()); ++row) {
            if (keyFn(currentRows[static_cast<std::size_t>(row)]) == *selectedKey) {
                tree->setCurrentItem(tree->topLevelItem(row));
                break;
            }
        }
    }
    restoreItemViewScrollSnapshot(tree, scrollSnapshot);
    return true;
}

} // namespace savorqt::gui
