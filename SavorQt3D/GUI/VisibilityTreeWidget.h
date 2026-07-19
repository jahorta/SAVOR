#pragma once

#include <QWidget>
#include <QVariant>

class QTreeWidget;
class QTreeWidgetItem;

namespace savor::qt3d::gui {

class VisibilityTreeWidget final : public QWidget {
    Q_OBJECT

public:
    enum class LayerKind {
        All = 0,
        Grounds = 1,
        Links = 2,
        Collisions = 3,
        Triggers = 4,
        MovingObjects = 5,
        Unknowns = 6,
    };

    explicit VisibilityTreeWidget(QWidget* parent = nullptr);

    void setLayers(const QVariantList& grounds,
        const QVariantList& links,
        const QVariantList& collisions,
        const QVariantList& triggers,
        const QVariantList& movingObjects,
        const QVariantList& unknowns);

signals:
    void allToggled(bool visible);
    void layerToggled(int layer, bool visible);
    void leafToggled(int layer, int index, bool visible);

private:
    void rebuildTree();
    void updateTriStateChecks();
    QString layerLabel(LayerKind layer) const;

    QTreeWidget* tree_ = nullptr;
    QTreeWidgetItem* allItem_ = nullptr;
    QTreeWidgetItem* groundsItem_ = nullptr;
    QTreeWidgetItem* linksItem_ = nullptr;
    QTreeWidgetItem* collisionsItem_ = nullptr;
    QTreeWidgetItem* triggersItem_ = nullptr;
    QTreeWidgetItem* movingObjectsItem_ = nullptr;
    QTreeWidgetItem* unknownsItem_ = nullptr;
    QVariantList grounds_{};
    QVariantList links_{};
    QVariantList collisions_{};
    QVariantList triggers_{};
    QVariantList movingObjects_{};
    QVariantList unknowns_{};
    bool updating_ = false;
};

} // namespace savor::qt3d::gui
