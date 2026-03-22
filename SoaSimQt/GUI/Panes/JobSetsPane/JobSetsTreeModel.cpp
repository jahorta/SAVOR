#include "JobSetsTreeModel.h"

#include <QtCore/QDateTime>

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace {

bool jobSetLess(const JobSetsTreeModel::Item& a, const JobSetsTreeModel::Item& b)
{
    if (a.jobSet.job_set_id != b.jobSet.job_set_id) {
        return a.jobSet.job_set_id > b.jobSet.job_set_id;
    }
    return a.jobSet.created_at > b.jobSet.created_at;
}

}

JobSetsTreeModel::JobSetsTreeModel(QObject* parent)
    : QAbstractItemModel(parent)
    , root_(std::make_unique<Node>())
{
}

int JobSetsTreeModel::rowCount(const QModelIndex& parent) const
{
    const Node* node = nodeFromIndex(parent);
    return node ? static_cast<int>(node->children.size()) : 0;
}

int JobSetsTreeModel::columnCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    return ColumnCount;
}

QModelIndex JobSetsTreeModel::index(int row, int column, const QModelIndex& parent) const
{
    if (column < 0 || column >= ColumnCount) {
        return {};
    }

    Node* parentNode = nodeFromIndex(parent);
    if (!parentNode || row < 0 || row >= static_cast<int>(parentNode->children.size())) {
        return {};
    }

    return createIndex(row, column, parentNode->children[static_cast<size_t>(row)].get());
}

QModelIndex JobSetsTreeModel::parent(const QModelIndex& child) const
{
    if (!child.isValid()) {
        return {};
    }

    Node* node = nodeFromIndex(child);
    if (!node || !node->parent || node->parent == root_.get()) {
        return {};
    }

    Node* parentNode = node->parent;
    Node* grandParent = parentNode->parent ? parentNode->parent : root_.get();
    const int row = rowOfChild(grandParent, parentNode);
    return row >= 0 ? createIndex(row, 0, parentNode) : QModelIndex{};
}

QVariant JobSetsTreeModel::data(const QModelIndex& index, int role) const
{
    Node* node = nodeFromIndex(index);
    if (!node || node == root_.get()) {
        return {};
    }

    const JobSetLite& row = node->item.jobSet;

    if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
        case JobSetIdColumn:
        case CreatedAtColumn:
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        default:
            break;
        }
    }

    if (role == JobSetIdRole) return static_cast<qlonglong>(row.job_set_id);
    if (role == TotalJobsRole) return static_cast<qlonglong>(row.total_jobs);
    if (role == CompletedJobsRole) return static_cast<qlonglong>(row.completed_jobs);
    if (role == SucceededJobsRole) return static_cast<qlonglong>(row.succeeded_jobs);
    if (role == FailedJobsRole) return static_cast<qlonglong>(row.failed_jobs);
    if (role == CanceledJobsRole) return static_cast<qlonglong>(row.canceled_jobs);

    if (role != Qt::DisplayRole) {
        return {};
    }

    switch (index.column()) {
    case JobSetIdColumn: return static_cast<qlonglong>(row.job_set_id);
    case ProgramKindColumn: return node->item.programKindName.isEmpty() ? QStringLiteral("kind %1").arg(row.program_kind) : node->item.programKindName;
    case PurposeColumn: return displayPurpose(row.purpose);
    case CreatedAtColumn: return formatTime(row.created_at);
    case ProgressColumn:
        return QStringLiteral("%1 / %2 complete").arg(row.completed_jobs).arg(row.total_jobs);
    case ActionsColumn:
        return QStringLiteral("Right-click");
    default:
        return {};
    }
}

QVariant JobSetsTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) {
        return QAbstractItemModel::headerData(section, orientation, role);
    }

    switch (section) {
    case JobSetIdColumn: return QStringLiteral("job_set_id");
    case ProgramKindColumn: return QStringLiteral("program_kind");
    case PurposeColumn: return QStringLiteral("purpose");
    case CreatedAtColumn: return QStringLiteral("created_at");
    case ProgressColumn: return QStringLiteral("progress");
    case ActionsColumn: return QStringLiteral("actions");
    default: return {};
    }
}

void JobSetsTreeModel::syncRows(const std::vector<JobSetLite>& rows, const QHash<int, QString>& programNames)
{
    QSet<qint64> incomingIds;
    incomingIds.reserve(static_cast<qsizetype>(rows.size()));
    std::unordered_map<qint64, const JobSetLite*> desiredById;
    desiredById.reserve(rows.size());

    for (const JobSetLite& row : rows) {
        incomingIds.insert(row.job_set_id);
        desiredById.emplace(row.job_set_id, &row);
    }

    std::vector<qint64> staleIds;
    staleIds.reserve(static_cast<size_t>(byId_.size()));
    for (auto it = byId_.cbegin(); it != byId_.cend(); ++it) {
        if (!incomingIds.contains(it.key())) {
            staleIds.push_back(it.key());
        }
    }

    std::sort(staleIds.begin(), staleIds.end(), [&](qint64 lhs, qint64 rhs) {
        return depthOf(byId_.value(lhs)) > depthOf(byId_.value(rhs));
    });

    for (qint64 staleId : staleIds) {
        Node* node = byId_.value(staleId, nullptr);
        if (node) {
            removeNode(node);
        }
    }

    std::vector<Node*> changedNodes;
    changedNodes.reserve(rows.size());
    for (const JobSetLite& row : rows) {
        Node* node = byId_.value(row.job_set_id, nullptr);
        if (!node) {
            continue;
        }

        Item incoming{row, programNames.value(row.program_kind)};
        const bool displayChanged = itemsAffectDisplay(node->item, incoming);
        node->item = std::move(incoming);
        if (displayChanged) {
            changedNodes.push_back(node);
        }
    }

    std::vector<const JobSetLite*> ordered;
    ordered.reserve(rows.size());
    for (const JobSetLite& row : rows) {
        ordered.push_back(&row);
    }
    std::sort(ordered.begin(), ordered.end(), [](const JobSetLite* a, const JobSetLite* b) {
        if (a->job_set_id != b->job_set_id) {
            return a->job_set_id > b->job_set_id;
        }
        return a->created_at > b->created_at;
    });

    for (const JobSetLite* row : ordered) {
        if (!byId_.contains(row->job_set_id)) {
            auto node = std::make_unique<Node>();
            node->item.jobSet = *row;
            node->item.programKindName = programNames.value(row->program_kind);
            insertNode(std::move(node), desiredParentFor(*row, incomingIds));
        }
    }

    std::vector<qint64> orderedIds;
    orderedIds.reserve(ordered.size());
    for (const JobSetLite* row : ordered) {
        orderedIds.push_back(row->job_set_id);
    }

    for (qint64 jobSetId : orderedIds) {
        Node* node = byId_.value(jobSetId, nullptr);
        const JobSetLite* desiredRow = desiredById[jobSetId];
        if (!node || !desiredRow) {
            continue;
        }

        Node* desiredParent = desiredParentFor(*desiredRow, incomingIds);
        if (node->parent != desiredParent) {
            moveNode(node, desiredParent);
            continue;
        }

        const int desiredRowIndex = insertionRowForParent(desiredParent, node->item, node);
        const int currentRow = rowOfChild(desiredParent, node);
        if (desiredRowIndex >= 0 && currentRow >= 0 && desiredRowIndex != currentRow) {
            moveNode(node, desiredParent);
        }
    }

    emitDataChangedBatches(changedNodes);
}

bool JobSetsTreeModel::containsJobSetId(qint64 jobSetId) const
{
    return byId_.contains(jobSetId);
}

QModelIndex JobSetsTreeModel::indexForJobSetId(qint64 jobSetId, int column) const
{
    if (column < 0 || column >= ColumnCount || !byId_.contains(jobSetId)) {
        return {};
    }

    Node* node = byId_.value(jobSetId);
    if (!node || !node->parent) {
        return {};
    }

    const int row = rowOfChild(node->parent, node);
    return row >= 0 ? createIndex(row, column, node) : QModelIndex{};
}

QString JobSetsTreeModel::formatTime(qint64 epochSeconds)
{
    if (epochSeconds <= 0) {
        return QStringLiteral("--");
    }
    return QDateTime::fromSecsSinceEpoch(epochSeconds).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString JobSetsTreeModel::displayPurpose(const std::string& purpose)
{
    return purpose.empty() ? QStringLiteral("(none)") : QString::fromStdString(purpose);
}

JobSetsTreeModel::Node* JobSetsTreeModel::nodeFromIndex(const QModelIndex& index) const
{
    if (!index.isValid()) {
        return root_.get();
    }
    return static_cast<Node*>(index.internalPointer());
}

int JobSetsTreeModel::rowOfChild(const Node* parent, const Node* child)
{
    if (!parent || !child) {
        return -1;
    }

    const int row = child->rowInParent;
    if (row < 0 || row >= static_cast<int>(parent->children.size())) {
        return -1;
    }

    return parent->children[static_cast<size_t>(row)].get() == child ? row : -1;
}

bool JobSetsTreeModel::itemsAffectDisplay(const Item& lhs, const Item& rhs)
{
    const JobSetLite& a = lhs.jobSet;
    const JobSetLite& b = rhs.jobSet;
    return a.job_set_id != b.job_set_id
        || a.program_kind != b.program_kind
        || lhs.programKindName != rhs.programKindName
        || a.purpose != b.purpose
        || a.created_at != b.created_at
        || a.total_jobs != b.total_jobs
        || a.completed_jobs != b.completed_jobs
        || a.succeeded_jobs != b.succeeded_jobs
        || a.failed_jobs != b.failed_jobs
        || a.canceled_jobs != b.canceled_jobs;
}

void JobSetsTreeModel::refreshChildRows(Node* parent, int startRow)
{
    if (!parent || startRow < 0) {
        return;
    }

    for (int row = startRow; row < static_cast<int>(parent->children.size()); ++row) {
        parent->children[static_cast<size_t>(row)]->rowInParent = row;
    }
}

QModelIndex JobSetsTreeModel::indexForNode(const Node* node, int column) const
{
    if (!node || node == root_.get() || !node->parent || column < 0 || column >= ColumnCount) {
        return {};
    }

    const int row = rowOfChild(node->parent, node);
    return row >= 0 ? createIndex(row, column, const_cast<Node*>(node)) : QModelIndex{};
}

int JobSetsTreeModel::depthOf(const Node* node) const
{
    int depth = 0;
    for (const Node* current = node; current && current->parent; current = current->parent) {
        ++depth;
    }
    return depth;
}

JobSetsTreeModel::Node* JobSetsTreeModel::desiredParentFor(const JobSetLite& row, const QSet<qint64>& incomingIds) const
{
    if (!row.parent_job_set_id.has_value() || !incomingIds.contains(*row.parent_job_set_id)) {
        return root_.get();
    }
    return byId_.value(*row.parent_job_set_id, root_.get());
}

int JobSetsTreeModel::insertionRowForParent(const Node* parent, const Item& item, const Node* skipNode) const
{
    if (!parent) {
        return 0;
    }

    int row = 0;
    for (const std::unique_ptr<Node>& child : parent->children) {
        if (child.get() == skipNode) {
            continue;
        }
        if (jobSetLess(item, child->item)) {
            ++row;
            continue;
        }
        break;
    }
    return row;
}

std::unique_ptr<JobSetsTreeModel::Node> JobSetsTreeModel::takeChild(Node* parent, int row)
{
    std::unique_ptr<Node> node = std::move(parent->children[static_cast<size_t>(row)]);
    parent->children.erase(parent->children.begin() + row);
    return node;
}

void JobSetsTreeModel::registerNodeRecursive(Node* node)
{
    if (!node) {
        return;
    }

    byId_.insert(node->item.jobSet.job_set_id, node);
    for (const std::unique_ptr<Node>& child : node->children) {
        child->parent = node;
        registerNodeRecursive(child.get());
    }
}

void JobSetsTreeModel::unregisterNodeRecursive(Node* node)
{
    if (!node) {
        return;
    }

    for (const std::unique_ptr<Node>& child : node->children) {
        unregisterNodeRecursive(child.get());
    }
    byId_.remove(node->item.jobSet.job_set_id);
}

void JobSetsTreeModel::insertNode(std::unique_ptr<Node> node, Node* parent)
{
    if (!node) {
        return;
    }

    if (!parent) {
        parent = root_.get();
    }

    const int row = insertionRowForParent(parent, node->item);
    QModelIndex parentIndex = indexForNode(parent);
    beginInsertRows(parentIndex, row, row);
    node->parent = parent;
    Node* rawNode = node.get();
    parent->children.insert(parent->children.begin() + row, std::move(node));
    refreshChildRows(parent, row);
    registerNodeRecursive(rawNode);
    endInsertRows();
}

void JobSetsTreeModel::removeNode(Node* node)
{
    if (!node || !node->parent) {
        return;
    }

    Node* parent = node->parent;
    const int row = rowOfChild(parent, node);
    if (row < 0) {
        return;
    }

    QModelIndex parentIndex = indexForNode(parent);
    beginRemoveRows(parentIndex, row, row);
    std::unique_ptr<Node> removed = takeChild(parent, row);
    refreshChildRows(parent, row);
    unregisterNodeRecursive(removed.get());
    endRemoveRows();
}

void JobSetsTreeModel::moveNode(Node* node, Node* newParent)
{
    if (!node || !node->parent) {
        return;
    }

    if (!newParent) {
        newParent = root_.get();
    }

    Node* oldParent = node->parent;
    const int oldRow = rowOfChild(oldParent, node);
    if (oldRow < 0) {
        return;
    }

    const int newRow = insertionRowForParent(newParent, node->item, node);
    if (oldParent == newParent && (newRow == oldRow || newRow == oldRow + 1)) {
        return;
    }

    QModelIndex oldParentIndex = indexForNode(oldParent);
    QModelIndex newParentIndex = indexForNode(newParent);
    if (!beginMoveRows(oldParentIndex, oldRow, oldRow, newParentIndex, newRow)) {
        return;
    }

    std::unique_ptr<Node> moved = takeChild(oldParent, oldRow);
    moved->parent = newParent;
    int adjustedNewRow = newRow;
    if (oldParent == newParent && newRow > oldRow) {
        adjustedNewRow -= 1;
    }
    newParent->children.insert(newParent->children.begin() + adjustedNewRow, std::move(moved));
    refreshChildRows(oldParent, oldRow);
    refreshChildRows(newParent, adjustedNewRow);
    endMoveRows();
}

void JobSetsTreeModel::emitDataChangedBatches(const std::vector<Node*>& changedNodes)
{
    if (changedNodes.empty()) {
        return;
    }

    std::vector<Node*> orderedNodes = changedNodes;
    std::sort(orderedNodes.begin(), orderedNodes.end(), [](const Node* lhs, const Node* rhs) {
        if (lhs->parent != rhs->parent) {
            return lhs->parent < rhs->parent;
        }
        return lhs->rowInParent < rhs->rowInParent;
    });
    orderedNodes.erase(std::unique(orderedNodes.begin(), orderedNodes.end()), orderedNodes.end());

    const Node* batchStart = nullptr;
    const Node* batchEnd = nullptr;
    for (const Node* node : orderedNodes) {
        if (!node || node == root_.get()) {
            continue;
        }

        if (!batchStart) {
            batchStart = batchEnd = node;
            continue;
        }

        if (batchEnd->parent == node->parent && node->rowInParent == batchEnd->rowInParent + 1) {
            batchEnd = node;
            continue;
        }

        const QModelIndex topLeft = indexForNode(batchStart, 0);
        const QModelIndex bottomRight = indexForNode(batchEnd, ColumnCount - 1);
        if (topLeft.isValid() && bottomRight.isValid()) {
            emit dataChanged(topLeft, bottomRight);
        }
        batchStart = batchEnd = node;
    }

    if (batchStart && batchEnd) {
        const QModelIndex topLeft = indexForNode(batchStart, 0);
        const QModelIndex bottomRight = indexForNode(batchEnd, ColumnCount - 1);
        if (topLeft.isValid() && bottomRight.isValid()) {
            emit dataChanged(topLeft, bottomRight);
        }
    }
}
