#include "ArtifactsTableView.h"

#include "ArtifactsTableModel.h"

#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QHeaderView>

ArtifactsTableView::ArtifactsTableView(QWidget* parent)
    : QTableView(parent)
{
    setObjectName("jobsArtifactsTable");
    verticalHeader()->setVisible(false);
    setSelectionMode(QAbstractItemView::NoSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setShowGrid(false);
    setAlternatingRowColors(true);
    horizontalHeader()->setStretchLastSection(true);
    horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
}

void ArtifactsTableView::attachModel(ArtifactsTableModel* model)
{
    setModel(model);
}

ArtifactsTableModel* ArtifactsTableView::artifactsModel()
{
    return static_cast<ArtifactsTableModel*>(model());
}

const ArtifactsTableModel* ArtifactsTableView::artifactsModel() const
{
    return static_cast<const ArtifactsTableModel*>(model());
}
