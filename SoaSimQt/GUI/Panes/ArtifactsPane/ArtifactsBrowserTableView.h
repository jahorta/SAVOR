#pragma once

#include <QtWidgets/QTreeView>

class ArtifactsBrowserTableModel;

class ArtifactsBrowserTableView final : public QTreeView
{
public:
    explicit ArtifactsBrowserTableView(QWidget* parent = nullptr);

    void attachModel(ArtifactsBrowserTableModel* model);
    ArtifactsBrowserTableModel* artifactsModel();
    const ArtifactsBrowserTableModel* artifactsModel() const;
};
