#include "ArtifactsBrowserTableView.h"

#include "ArtifactsBrowserTableModel.h"

#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QHeaderView>

ArtifactsBrowserTableView::ArtifactsBrowserTableView(QWidget* parent)
    : QTableView(parent)
{
    setObjectName("jobsTable");
    verticalHeader()->setVisible(false);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setShowGrid(false);
    setAlternatingRowColors(true);
    setSortingEnabled(false);
    horizontalHeader()->setStretchLastSection(false);
    horizontalHeader()->setHighlightSections(false);
}

void ArtifactsBrowserTableView::attachModel(ArtifactsBrowserTableModel* model)
{
    setModel(model);
    horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
}

ArtifactsBrowserTableModel* ArtifactsBrowserTableView::artifactsModel()
{
    return static_cast<ArtifactsBrowserTableModel*>(model());
}

const ArtifactsBrowserTableModel* ArtifactsBrowserTableView::artifactsModel() const
{
    return static_cast<const ArtifactsBrowserTableModel*>(model());
}
