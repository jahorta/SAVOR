#pragma once

#include <QtCore/QStringList>
#include <QtWidgets/QTreeView>

class ArtifactsBrowserTableModel;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;

class ArtifactsBrowserTableView final : public QTreeView
{
    Q_OBJECT

public:
    explicit ArtifactsBrowserTableView(QWidget* parent = nullptr);

    void attachModel(ArtifactsBrowserTableModel* model);
    ArtifactsBrowserTableModel* artifactsModel();
    const ArtifactsBrowserTableModel* artifactsModel() const;

signals:
    void fileDropRequested(const QStringList& paths);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
};
