#include "ArtifactsTableView.h"

#include "ArtifactsTableModel.h"

#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QHeaderView>

ArtifactsTableView::ArtifactsTableView(QWidget* parent)
    : QTreeView(parent)
{
    setObjectName("jobsArtifactsTable");
    setSelectionMode(QAbstractItemView::NoSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setAlternatingRowColors(true);
    setRootIsDecorated(false);
    setItemsExpandable(false);
    setAllColumnsShowFocus(true);
    setUniformRowHeights(true);
    setIndentation(0);
    header()->setStretchLastSection(true);
}

void ArtifactsTableView::attachModel(ArtifactsTableModel* model)
{
    setModel(model);

    header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
}

ArtifactsTableModel* ArtifactsTableView::artifactsModel()
{
    return static_cast<ArtifactsTableModel*>(model());
}

const ArtifactsTableModel* ArtifactsTableView::artifactsModel() const
{
    return static_cast<const ArtifactsTableModel*>(model());
}
