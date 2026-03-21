#pragma once

#include <QtWidgets/QTableView>

class ArtifactsBrowserTableModel;

class ArtifactsBrowserTableView final : public QTableView
{
public:
    explicit ArtifactsBrowserTableView(QWidget* parent = nullptr);

    void attachModel(ArtifactsBrowserTableModel* model);
    ArtifactsBrowserTableModel* artifactsModel();
    const ArtifactsBrowserTableModel* artifactsModel() const;
};
