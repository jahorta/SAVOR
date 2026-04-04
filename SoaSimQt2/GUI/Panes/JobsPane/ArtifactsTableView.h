#pragma once

#include <QtWidgets/QTreeView>

class ArtifactsTableModel;

class ArtifactsTableView final : public QTreeView
{
public:
    explicit ArtifactsTableView(QWidget* parent = nullptr);

    void attachModel(ArtifactsTableModel* model);
    ArtifactsTableModel* artifactsModel();
    const ArtifactsTableModel* artifactsModel() const;
};
