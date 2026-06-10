#include "ArtifactsBrowserTableView.h"

#include "ArtifactsBrowserTableModel.h"

#include <QtCore/QMimeData>
#include <QtCore/QUrl>
#include <QtWidgets/QAbstractItemView>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtWidgets/QHeaderView>

ArtifactsBrowserTableView::ArtifactsBrowserTableView(QWidget* parent)
    : QTreeView(parent)
{
    setObjectName("jobsTable");
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setAlternatingRowColors(true);
    setSortingEnabled(false);
    setRootIsDecorated(false);
    setItemsExpandable(false);
    setAllColumnsShowFocus(true);
    setUniformRowHeights(true);
    setIndentation(0);
    header()->setStretchLastSection(false);
    header()->setHighlightSections(false);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
}

void ArtifactsBrowserTableView::attachModel(ArtifactsBrowserTableModel* model)
{
    setModel(model);
    header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    header()->setSectionResizeMode(1, QHeaderView::Stretch);
    header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    header()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
}

ArtifactsBrowserTableModel* ArtifactsBrowserTableView::artifactsModel()
{
    return static_cast<ArtifactsBrowserTableModel*>(model());
}

const ArtifactsBrowserTableModel* ArtifactsBrowserTableView::artifactsModel() const
{
    return static_cast<const ArtifactsBrowserTableModel*>(model());
}

void ArtifactsBrowserTableView::dragEnterEvent(QDragEnterEvent* event)
{
    if (event && event->mimeData() && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    QTreeView::dragEnterEvent(event);
}

void ArtifactsBrowserTableView::dragMoveEvent(QDragMoveEvent* event)
{
    if (event && event->mimeData() && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    QTreeView::dragMoveEvent(event);
}

void ArtifactsBrowserTableView::dropEvent(QDropEvent* event)
{
    if (!event || !event->mimeData() || !event->mimeData()->hasUrls()) {
        QTreeView::dropEvent(event);
        return;
    }

    QStringList paths;
    const QList<QUrl> urls = event->mimeData()->urls();
    for (const QUrl& url : urls) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        }
    }

    if (!paths.isEmpty()) {
        emit fileDropRequested(paths);
        event->acceptProposedAction();
        return;
    }

    QTreeView::dropEvent(event);
}
