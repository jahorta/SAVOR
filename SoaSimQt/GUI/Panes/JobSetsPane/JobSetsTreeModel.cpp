#include "JobSetsTreeModel.h"

#include <QtCore/QDateTime>

#include <algorithm>
#include <unordered_map>


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

void JobSetsTreeModel::setRows(const std::vector<JobSetLite>& rows, const QHash<int, QString>& programNames)
{
    beginResetModel();
    root_ = std::make_unique<Node>();
    byId_.clear();

    std::vector<const JobSetLite*> ordered;
    ordered.reserve(rows.size());
    for (const JobSetLite& row : rows) {
        ordered.push_back(&row);
    }

    std::sort(ordered.begin(), ordered.end(), [](const JobSetLite* a, const JobSetLite* b) {
        if (a->created_at != b->created_at) {
            return a->created_at > b->created_at;
        }
        return a->job_set_id > b->job_set_id;
    });

    std::unordered_map<qint64, std::unique_ptr<Node>> pending;
    pending.reserve(rows.size());

    for (const JobSetLite* row : ordered) {
        auto node = std::make_unique<Node>();
        node->item.jobSet = *row;
        node->item.programKindName = programNames.value(row->program_kind);
        byId_.insert(row->job_set_id, node.get());
        pending.emplace(row->job_set_id, std::move(node));
    }

    for (const JobSetLite* row : ordered) {
        std::unique_ptr<Node> node = std::move(pending[row->job_set_id]);
        Node* parentNode = root_.get();
        if (row->parent_job_set_id.has_value()) {
            auto parentIt = byId_.find(*row->parent_job_set_id);
            if (parentIt != byId_.end()) {
                parentNode = parentIt.value();
            }
        }
        node->parent = parentNode;
        parentNode->children.push_back(std::move(node));
    }

    std::function<void(Node*)> sortChildren = [&](Node* node) {
        std::sort(node->children.begin(), node->children.end(), [](const std::unique_ptr<Node>& a, const std::unique_ptr<Node>& b) {
            if (a->item.jobSet.created_at != b->item.jobSet.created_at) {
                return a->item.jobSet.created_at > b->item.jobSet.created_at;
            }
            return a->item.jobSet.job_set_id > b->item.jobSet.job_set_id;
        });
        for (const std::unique_ptr<Node>& child : node->children) {
            sortChildren(child.get());
        }
    };
    sortChildren(root_.get());

    endResetModel();
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

    for (int row = 0; row < static_cast<int>(parent->children.size()); ++row) {
        if (parent->children[static_cast<size_t>(row)].get() == child) {
            return row;
        }
    }

    return -1;
}
