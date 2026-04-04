#pragma once

#include <QtCore/QAbstractItemModel>
#include <QtCore/QHash>
#include <QtCore/QSet>

#include "DB/Querying/JobSetListDTO.h"

#include <functional>
#include <memory>
#include <vector>

class JobSetsTreeModel final : public QAbstractItemModel
{
public:
    struct Item {
        JobSetLite jobSet;
        QString programKindName;
    };

    enum Column {
        JobSetIdColumn = 0,
        ProgramKindColumn,
        PurposeColumn,
        CreatedAtColumn,
        ProgressColumn,
        ActionsColumn,
        ColumnCount
    };

    enum Role {
        JobSetIdRole = Qt::UserRole + 1,
        TotalJobsRole,
        CompletedJobsRole,
        SucceededJobsRole,
        FailedJobsRole,
        CanceledJobsRole
    };

    explicit JobSetsTreeModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void syncRows(const std::vector<JobSetLite>& rows, const QHash<int, QString>& programNames);
    bool containsJobSetId(qint64 jobSetId) const;
    QModelIndex indexForJobSetId(qint64 jobSetId, int column = 0) const;

private:
    struct Node {
        Item item;
        Node* parent = nullptr;
        std::vector<std::unique_ptr<Node>> children;
        int rowInParent = -1;
    };

    static QString formatTime(qint64 epochSeconds);
    static QString displayPurpose(const std::string& purpose);

    Node* nodeFromIndex(const QModelIndex& index) const;
    static int rowOfChild(const Node* parent, const Node* child);
    static bool itemsAffectDisplay(const Item& lhs, const Item& rhs);
    static void refreshChildRows(Node* parent, int startRow = 0);

    QModelIndex indexForNode(const Node* node, int column = 0) const;
    int depthOf(const Node* node) const;
    Node* desiredParentFor(const JobSetLite& row, const QSet<qint64>& incomingIds) const;
    int insertionRowForParent(const Node* parent, const Item& item, const Node* skipNode = nullptr) const;
    static std::unique_ptr<Node> takeChild(Node* parent, int row);
    void registerNodeRecursive(Node* node);
    void unregisterNodeRecursive(Node* node);
    void insertNode(std::unique_ptr<Node> node, Node* parent);
    void removeNode(Node* node);
    void moveNode(Node* node, Node* newParent);
    void emitDataChangedBatches(const std::vector<Node*>& changedNodes);

    std::unique_ptr<Node> root_;
    QHash<qint64, Node*> byId_;
};
