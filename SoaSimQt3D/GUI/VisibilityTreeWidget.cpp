#include "VisibilityTreeWidget.h"

#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace soasim::qt3d::gui {
namespace {

constexpr int kLayerRole = Qt::UserRole + 1;
constexpr int kLeafIndexRole = Qt::UserRole + 2;

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

    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (item == nullptr || column != 0 || updating_) {
            return;
        }

        const LayerKind layer = static_cast<LayerKind>(item->data(0, kLayerRole).toInt());
        const int leafIndex = item->data(0, kLeafIndexRole).toInt();
        const Qt::CheckState state = item->checkState(0);

        if (state == Qt::PartiallyChecked) {
            updateTriStateChecks();
            return;
        }

        const bool checked = state == Qt::Checked;

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
    const QVariantList& collisions,
    const QVariantList& triggers,
    const QVariantList& unknowns) {
    grounds_ = grounds;
    links_ = links;
    collisions_ = collisions;
    triggers_ = triggers;
    unknowns_ = unknowns;
    rebuildTree();
}

void VisibilityTreeWidget::rebuildTree() {
    const QSignalBlocker blocker(tree_);
    updating_ = true;

    tree_->clear();

    auto makeNode = [](const QString& text, const LayerKind layer, const int leafIndex) {
        auto* item = new QTreeWidgetItem(QStringList(text));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Checked);
        item->setData(0, kLayerRole, static_cast<int>(layer));
        item->setData(0, kLeafIndexRole, leafIndex);
        return item;
    };

    allItem_ = makeNode(QStringLiteral("All"), LayerKind::All, -1);
    tree_->addTopLevelItem(allItem_);

    auto buildLayer = [&](const LayerKind layer, const QVariantList& meshes, QTreeWidgetItem*& outItem) {
        outItem = makeNode(layerLabel(layer), layer, -1);
        allItem_->addChild(outItem);

        for (int i = 0; i < meshes.size(); ++i) {
            const QVariantMap mesh = meshes.at(i).toMap();
            const QString label = mesh.value("label").toString();
            auto* leaf = makeNode(label.isEmpty() ? QStringLiteral("(unnamed)") : label, layer, i);
            leaf->setCheckState(0, mesh.value("visible", true).toBool() ? Qt::Checked : Qt::Unchecked);
            outItem->addChild(leaf);
        }
    };

    buildLayer(LayerKind::Grounds, grounds_, groundsItem_);
    buildLayer(LayerKind::Links, links_, linksItem_);
    buildLayer(LayerKind::Collisions, collisions_, collisionsItem_);
    buildLayer(LayerKind::Triggers, triggers_, triggersItem_);
    buildLayer(LayerKind::Unknowns, unknowns_, unknownsItem_);

    tree_->expandItem(allItem_);
    tree_->expandItem(groundsItem_);
    tree_->expandItem(linksItem_);
    tree_->expandItem(collisionsItem_);
    tree_->expandItem(triggersItem_);
    tree_->expandItem(unknownsItem_);

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
    updateGroup(collisionsItem_);
    updateGroup(triggersItem_);
    updateGroup(unknownsItem_);

    int checkedGroups = 0;
    int partialGroups = 0;
    const auto observe = [&](QTreeWidgetItem* item) {
        if (item == nullptr) {
            return;
        }
        if (item->checkState(0) == Qt::Checked) {
            ++checkedGroups;
        } else if (item->checkState(0) == Qt::PartiallyChecked) {
            ++partialGroups;
        }
    };
    observe(groundsItem_);
    observe(linksItem_);
    observe(collisionsItem_);
    observe(triggersItem_);
    observe(unknownsItem_);

    if (allItem_ != nullptr) {
        if (checkedGroups == 0 && partialGroups == 0) {
            allItem_->setCheckState(0, Qt::Unchecked);
        } else if (checkedGroups == 5 && partialGroups == 0) {
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
    case LayerKind::Collisions:
        return QStringLiteral("Collisions");
    case LayerKind::Triggers:
        return QStringLiteral("Triggers");
    case LayerKind::Unknowns:
        return QStringLiteral("Unknowns");
    default:
        return QStringLiteral("All");
    }
}

} // namespace soasim::qt3d::gui
