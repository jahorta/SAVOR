#include "ArtifactsBrowserTableView.h"

#include "ArtifactsBrowserTableModel.h"

#include <QtWidgets/QAbstractItemView>
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
