#pragma once

#include <QtWidgets/QTableView>

class ArtifactsTableModel;

class ArtifactsTableView final : public QTableView
{
public:
    explicit ArtifactsTableView(QWidget* parent = nullptr);

    void attachModel(ArtifactsTableModel* model);
    ArtifactsTableModel* artifactsModel();
    const ArtifactsTableModel* artifactsModel() const;
};
