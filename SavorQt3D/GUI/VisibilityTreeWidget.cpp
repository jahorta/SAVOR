#include "VisibilityTreeWidget.h"

#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace savor::qt3d::gui {
namespace {

constexpr int kLayerRole = Qt::UserRole + 1;
} // namespace

VisibilityTreeWidget::VisibilityTreeWidget(QWidget* parent)
    : QWidget(parent)
    , tree_(new QTreeWidget(this)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tree_);

    tree_->setHeaderLabel(QStringLiteral("Rendered Object Visibility"));
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    tree_->setRootIsDecorated(true);

    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int column) {
        if (item == nullptr || column != 0 || updating_) {
            return;
        }

        const bool isLayerItem = item == allItem_ || item->parent() == allItem_;
        const bool isLeaf = !isLayerItem;
        QTreeWidgetItem* layerItem = item;
        int leafIndex = -1;
        if (isLeaf) {
            layerItem = item->parent();
            if (layerItem != nullptr) {
                leafIndex = layerItem->indexOfChild(item);
            }
        }

        const LayerKind layer = static_cast<LayerKind>((layerItem != nullptr)
                ? layerItem->data(0, kLayerRole).toInt()
                : static_cast<int>(LayerKind::All));
        const Qt::CheckState state = item->checkState(0);

        // A partial parent click means "show this group". Returning here would
        // leave partially visible groups, including All, impossible to toggle.
        const bool checked = state != Qt::Unchecked;

        if (leafIndex < 0) {
            const QSignalBlocker blocker(tree_);
            updating_ = true;
            for (int i = 0; i < item->childCount(); ++i) {
                item->child(i)->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
            }
            updating_ = false;
        }

        if (layer == LayerKind::All && leafIndex < 0) {
            emit allToggled(checked);
        } else if (leafIndex < 0) {
            emit layerToggled(static_cast<int>(layer), checked);
        } else {
            emit leafToggled(static_cast<int>(layer), leafIndex, checked);
        }

        updateTriStateChecks();
    });
}

void VisibilityTreeWidget::setLayers(const QVariantList& grounds,
    const QVariantList& links,
    const QVariantList& routes,
    const QVariantList& collisions,
    const QVariantList& triggers,
    const QVariantList& movingObjects,
    const QVariantList& unknowns) {
    grounds_ = grounds;
    links_ = links;
    routes_ = routes;
    collisions_ = collisions;
    triggers_ = triggers;
    movingObjects_ = movingObjects;
    unknowns_ = unknowns;
    rebuildTree();
}

void VisibilityTreeWidget::rebuildTree() {
    const QSignalBlocker blocker(tree_);
    updating_ = true;

    const bool hadExistingTree = allItem_ != nullptr;
    const bool allExpanded = allItem_ != nullptr && allItem_->isExpanded();
    const bool groundsExpanded = groundsItem_ != nullptr && groundsItem_->isExpanded();
    const bool linksExpanded = linksItem_ != nullptr && linksItem_->isExpanded();
    const bool routeExpanded = routeItem_ != nullptr && routeItem_->isExpanded();
    const bool collisionsExpanded = collisionsItem_ != nullptr && collisionsItem_->isExpanded();
    const bool triggersExpanded = triggersItem_ != nullptr && triggersItem_->isExpanded();
    const bool movingObjectsExpanded = movingObjectsItem_ != nullptr && movingObjectsItem_->isExpanded();
    const bool unknownsExpanded = unknownsItem_ != nullptr && unknownsItem_->isExpanded();

    tree_->clear();

    auto makeNode = [](const QString& text, const LayerKind layer) {
        auto* item = new QTreeWidgetItem(QStringList(text));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Checked);
        item->setData(0, kLayerRole, static_cast<int>(layer));
        return item;
    };

    allItem_ = makeNode(QStringLiteral("All"), LayerKind::All);
    tree_->addTopLevelItem(allItem_);

    auto buildLayer = [&](const LayerKind layer, const QVariantList& meshes, QTreeWidgetItem*& outItem) {
        outItem = makeNode(layerLabel(layer), layer);
        allItem_->addChild(outItem);

        for (int i = 0; i < meshes.size(); ++i) {
            const QVariantMap mesh = meshes.at(i).toMap();
            const QString label = mesh.value("label").toString();
            auto* leaf = makeNode(label.isEmpty() ? QStringLiteral("(unnamed)") : label, layer);
            leaf->setCheckState(0, mesh.value("visible", true).toBool() ? Qt::Checked : Qt::Unchecked);
            outItem->addChild(leaf);
        }
    };

    buildLayer(LayerKind::Grounds, grounds_, groundsItem_);
    buildLayer(LayerKind::Links, links_, linksItem_);
    buildLayer(LayerKind::Route, routes_, routeItem_);
    buildLayer(LayerKind::Collisions, collisions_, collisionsItem_);
    buildLayer(LayerKind::Triggers, triggers_, triggersItem_);
    buildLayer(LayerKind::MovingObjects, movingObjects_, movingObjectsItem_);
    buildLayer(LayerKind::Unknowns, unknowns_, unknownsItem_);

    allItem_->setExpanded(!hadExistingTree || allExpanded);
    groundsItem_->setExpanded(!hadExistingTree || groundsExpanded);
    linksItem_->setExpanded(!hadExistingTree || linksExpanded);
    routeItem_->setExpanded(!hadExistingTree || routeExpanded);
    collisionsItem_->setExpanded(!hadExistingTree || collisionsExpanded);
    triggersItem_->setExpanded(!hadExistingTree || triggersExpanded);
    movingObjectsItem_->setExpanded(!hadExistingTree || movingObjectsExpanded);
    unknownsItem_->setExpanded(!hadExistingTree || unknownsExpanded);

    updating_ = false;
    updateTriStateChecks();
}

void VisibilityTreeWidget::updateTriStateChecks() {
    const QSignalBlocker blocker(tree_);
    updating_ = true;

    auto updateGroup = [](QTreeWidgetItem* item) {
        if (item == nullptr) {
            return;
        }

        const int total = item->childCount();
        int checked = 0;
        for (int i = 0; i < total; ++i) {
            if (item->child(i)->checkState(0) == Qt::Checked) {
                ++checked;
            }
        }

        if (checked == 0) {
            item->setCheckState(0, Qt::Unchecked);
        } else if (checked == total) {
            item->setCheckState(0, Qt::Checked);
        } else {
            item->setCheckState(0, Qt::PartiallyChecked);
        }
    };

    updateGroup(groundsItem_);
    updateGroup(linksItem_);
    updateGroup(routeItem_);
    updateGroup(collisionsItem_);
    updateGroup(triggersItem_);
    updateGroup(movingObjectsItem_);
    updateGroup(unknownsItem_);

    int checkedGroups = 0;
    int partialGroups = 0;
    int nonEmptyGroups = 0;
    const auto observe = [&](QTreeWidgetItem* item) {
        if (item == nullptr || item->childCount() == 0) {
            return;
        }
        ++nonEmptyGroups;
        if (item->checkState(0) == Qt::Checked) {
            ++checkedGroups;
        } else if (item->checkState(0) == Qt::PartiallyChecked) {
            ++partialGroups;
        }
    };
    observe(groundsItem_);
    observe(linksItem_);
    observe(routeItem_);
    observe(collisionsItem_);
    observe(triggersItem_);
    observe(movingObjectsItem_);
    observe(unknownsItem_);

    if (allItem_ != nullptr) {
        if (checkedGroups == 0 && partialGroups == 0) {
            allItem_->setCheckState(0, Qt::Unchecked);
        } else if (nonEmptyGroups > 0 && checkedGroups == nonEmptyGroups && partialGroups == 0) {
            allItem_->setCheckState(0, Qt::Checked);
        } else {
            allItem_->setCheckState(0, Qt::PartiallyChecked);
        }
    }

    updating_ = false;
}

QString VisibilityTreeWidget::layerLabel(const LayerKind layer) const {
    switch (layer) {
    case LayerKind::Grounds:
        return QStringLiteral("Grounds");
    case LayerKind::Links:
        return QStringLiteral("Links");
    case LayerKind::Route:
        return QStringLiteral("Route");
    case LayerKind::Collisions:
        return QStringLiteral("Collisions");
    case LayerKind::Triggers:
        return QStringLiteral("Triggers");
    case LayerKind::MovingObjects:
        return QStringLiteral("MovingObjects");
    case LayerKind::Unknowns:
        return QStringLiteral("Unknowns");
    default:
        return QStringLiteral("All");
    }
}

} // namespace savor::qt3d::gui
