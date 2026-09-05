#pragma once

#include <QAbstractItemModel>
#include <QAbstractTableModel>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace savorqt::gui {

struct ProjectionModelFailure
{
    enum class Code {
        DuplicateKey,
        InvalidTreeParentage,
    };

    Code code = Code::DuplicateKey;
    QString modelIdentity;
    int firstOrdinal = -1;
    int conflictingOrdinal = -1;
};

template <typename Row, typename Key>
class KeyedTableProjectionModel final : public QAbstractTableModel
{
public:
    using KeyFunction = std::function<Key(const Row&)>;
    using EqualFunction = std::function<bool(const Row&, const Row&)>;
    using DataFunction = std::function<QVariant(const Row&, int, int)>;
    using FlagsFunction = std::function<Qt::ItemFlags(const Row&, int)>;

    KeyedTableProjectionModel(
        QString modelIdentity,
        QStringList headers,
        KeyFunction keyFunction,
        EqualFunction equalFunction,
        DataFunction dataFunction,
        QObject* parent = nullptr)
        : QAbstractTableModel(parent)
        , modelIdentity_(std::move(modelIdentity))
        , headers_(std::move(headers))
        , keyFunction_(std::move(keyFunction))
        , equalFunction_(std::move(equalFunction))
        , dataFunction_(std::move(dataFunction))
    {
    }

    int rowCount(const QModelIndex& parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }

    int columnCount(const QModelIndex& parent = {}) const override
    {
        return parent.isValid() ? 0 : headers_.size();
    }

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()
            || index.column() < 0 || index.column() >= columnCount()) {
            return {};
        }
        return dataFunction_(rows_[static_cast<std::size_t>(index.row())], index.column(), role);
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole
            && section >= 0 && section < headers_.size()) {
            return headers_[section];
        }
        return QAbstractTableModel::headerData(section, orientation, role);
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override
    {
        return index.isValid()
            ? Qt::ItemIsEnabled | Qt::ItemIsSelectable
            : Qt::NoItemFlags;
    }

    bool replaceRows(const std::vector<Row>& rows, ProjectionModelFailure* failureOut = nullptr)
    {
        if (failureOut != nullptr) *failureOut = {};
        for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
            const Key key = keyFunction_(rows[static_cast<std::size_t>(row)]);
            for (int prior = 0; prior < row; ++prior) {
                if (keyFunction_(rows[static_cast<std::size_t>(prior)]) == key) {
                    if (failureOut != nullptr) {
                        *failureOut = {
                            ProjectionModelFailure::Code::DuplicateKey,
                            modelIdentity_,
                            prior,
                            row,
                        };
                    }
                    return false;
                }
            }
        }
        if (rows_.size() == rows.size()) {
            bool equal = true;
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (!equalFunction_(rows_[i], rows[i])) {
                    equal = false;
                    break;
                }
            }
            if (equal) return true;
        }
        beginResetModel();
        rows_ = rows;
        endResetModel();
        return true;
    }

    [[nodiscard]] std::optional<Key> keyForIndex(const QModelIndex& index) const
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return std::nullopt;
        return keyFunction_(rows_[static_cast<std::size_t>(index.row())]);
    }

    [[nodiscard]] QModelIndex indexForKey(const Key& key) const
    {
        for (int row = 0; row < rowCount(); ++row) {
            if (keyFunction_(rows_[static_cast<std::size_t>(row)]) == key)
                return index(row, 0);
        }
        return {};
    }

    [[nodiscard]] const Row* rowAt(int row) const
    {
        return row >= 0 && row < rowCount() ? &rows_[static_cast<std::size_t>(row)] : nullptr;
    }

private:
    QString modelIdentity_;
    QStringList headers_;
    KeyFunction keyFunction_;
    EqualFunction equalFunction_;
    DataFunction dataFunction_;
    std::vector<Row> rows_;
};

template <typename Key>
struct KeyedTreeProjectionNode
{
    Key key;
    std::vector<QHash<int, QVariant>> columns;
    std::vector<KeyedTreeProjectionNode> children;
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
};

template <typename Key>
class KeyedTreeProjectionModel final : public QAbstractItemModel
{
public:
    using Node = KeyedTreeProjectionNode<Key>;

    explicit KeyedTreeProjectionModel(QString modelIdentity, QStringList headers, QObject* parent = nullptr)
        : QAbstractItemModel(parent)
        , modelIdentity_(std::move(modelIdentity))
        , headers_(std::move(headers))
    {
    }

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override
    {
        const auto* siblings = childrenFor(parent);
        if (siblings == nullptr || row < 0 || row >= static_cast<int>(siblings->size())
            || column < 0 || column >= columnCount()) return {};
        return createIndex(row, column, const_cast<Node*>(&(*siblings)[static_cast<std::size_t>(row)]));
    }

    QModelIndex parent(const QModelIndex& child) const override
    {
        if (!child.isValid()) return {};
        return parentFor(static_cast<const Node*>(child.internalPointer()), roots_, {});
    }

    int rowCount(const QModelIndex& parent = {}) const override
    {
        const auto* children = childrenFor(parent);
        return children == nullptr ? 0 : static_cast<int>(children->size());
    }

    int columnCount(const QModelIndex& = {}) const override { return headers_.size(); }

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid()) return {};
        const auto* node = static_cast<const Node*>(index.internalPointer());
        if (node == nullptr || index.column() < 0
            || index.column() >= static_cast<int>(node->columns.size())) return {};
        return node->columns[static_cast<std::size_t>(index.column())].value(role);
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole
            && section >= 0 && section < headers_.size()) return headers_[section];
        return QAbstractItemModel::headerData(section, orientation, role);
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override
    {
        const auto* node = index.isValid() ? static_cast<const Node*>(index.internalPointer()) : nullptr;
        return node == nullptr ? Qt::NoItemFlags : node->flags;
    }

    bool replaceRoots(std::vector<Node> roots, ProjectionModelFailure* failureOut = nullptr)
    {
        if (failureOut != nullptr) *failureOut = {};
        std::vector<const Node*> flattened;
        flatten(roots, flattened);
        for (int row = 0; row < static_cast<int>(flattened.size()); ++row) {
            for (int prior = 0; prior < row; ++prior) {
                if (flattened[static_cast<std::size_t>(row)]->key
                    == flattened[static_cast<std::size_t>(prior)]->key) {
                    if (failureOut != nullptr) {
                        *failureOut = {ProjectionModelFailure::Code::DuplicateKey,
                            modelIdentity_, prior, row};
                    }
                    return false;
                }
            }
        }
        beginResetModel();
        roots_ = std::move(roots);
        endResetModel();
        return true;
    }

private:
    const std::vector<Node>* childrenFor(const QModelIndex& parent) const
    {
        if (!parent.isValid()) return &roots_;
        const auto* node = static_cast<const Node*>(parent.internalPointer());
        return node == nullptr ? nullptr : &node->children;
    }

    QModelIndex parentFor(const Node* target, const std::vector<Node>& nodes, const QModelIndex& parentIndex) const
    {
        for (int row = 0; row < static_cast<int>(nodes.size()); ++row) {
            const Node& node = nodes[static_cast<std::size_t>(row)];
            for (const Node& child : node.children) {
                if (&child == target) return index(row, 0, parentIndex);
            }
            const QModelIndex nodeIndex = index(row, 0, parentIndex);
            const QModelIndex found = parentFor(target, node.children, nodeIndex);
            if (found.isValid()) return found;
        }
        return {};
    }

    static void flatten(const std::vector<Node>& nodes, std::vector<const Node*>& out)
    {
        for (const Node& node : nodes) {
            out.push_back(&node);
            flatten(node.children, out);
        }
    }

    QString modelIdentity_;
    QStringList headers_;
    std::vector<Node> roots_;
};

} // namespace savorqt::gui
