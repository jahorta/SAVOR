#include "TasRoutesTab.h"

#include "DB/SavorDbTasRouteService.h"
#include "GUI/Refresh/AsyncRefreshPipeline.h"

#include <QtCore/QTimer>
#include <QtGui/QBrush>
#include <QtGui/QKeyEvent>
#include <QtGui/QPen>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGraphicsItem>
#include <QtWidgets/QGraphicsScene>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <map>
#include <memory>
#include <unordered_map>

namespace {
class RouteView final : public QGraphicsView {
public:
    std::function<void()> activate;
    using QGraphicsView::QGraphicsView;
protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        QGraphicsView::mouseDoubleClickEvent(event);
        if (activate) activate();
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
            if (activate) activate();
            event->accept();
            return;
        }
        QGraphicsView::keyPressEvent(event);
    }
};
}

TasRoutesTab::TasRoutesTab(Actions actions, QWidget* parent)
    : WorkspacePageShell(QStringLiteral("tas-routes"), QStringLiteral("TAS Routes"),
          QStringLiteral("Semantic checkpoint and activity history"), parent),
      actions_(std::move(actions)) {
    auto* controls = new QHBoxLayout();
    rootFilter_ = new QComboBox(this);
    rootFilter_->addItem(QStringLiteral("All route roots"), 0);
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(QStringLiteral("Filter routes"));
    auto* fit = new QPushButton(QStringLiteral("Fit"), this);
    auto* refreshButton = new QPushButton(QStringLiteral("Refresh"), this);
    controls->addWidget(rootFilter_);
    controls->addWidget(search_, 1);
    controls->addWidget(fit);
    controls->addWidget(refreshButton);
    canvasLayout()->addLayout(controls);

    summary_ = new QLabel(QStringLiteral("No TAS routes have been recorded."), this);
    summary_->setObjectName("sectionDescription");
    canvasLayout()->addWidget(summary_);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    scene_ = new QGraphicsScene(splitter);
    auto* routeView = new RouteView(scene_, splitter);
    view_ = routeView;
    view_->setRenderHint(QPainter::Antialiasing);
    view_->setDragMode(QGraphicsView::ScrollHandDrag);
    view_->setMinimumWidth(680);
    auto* inspector = new QWidget(splitter);
    auto* inspectorLayout = new QVBoxLayout(inspector);
    auto* title = new QLabel(QStringLiteral("Route node"), inspector);
    title->setObjectName("panelTitle");
    detail_ = new QLabel(QStringLiteral("Select a checkpoint or activity."), inspector);
    detail_->setWordWrap(true);
    labelEdit_ = new QLineEdit(inspector);
    labelEdit_->setPlaceholderText(QStringLiteral("Node label"));
    auto* saveLabel = new QPushButton(QStringLiteral("Save label"), inspector);
    openVictories_ = new QPushButton(QStringLiteral("Open Victory Results"), inspector);
    openVictories_->setEnabled(false);
    inspectorLayout->addWidget(title);
    inspectorLayout->addWidget(detail_);
    inspectorLayout->addWidget(labelEdit_);
    inspectorLayout->addWidget(saveLabel);
    inspectorLayout->addWidget(openVictories_);
    inspectorLayout->addStretch();
    splitter->addWidget(routeView);
    splitter->addWidget(inspector);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 1);
    canvasLayout()->addWidget(splitter, 1);

    refresh_ = new savorqt::gui::AsyncRefreshPipeline<int, savorqt::db::TasRouteSnapshot>(this);
    refresh_->setAutoRefreshEnabled(true);
    refresh_->setRequestBuilder([](savorqt::gui::RefreshReason) { return 0; });
    refresh_->setLoadAndPrepare([](int) {
        return savorqt::gui::AsyncRefreshResult<
            savorqt::db::TasRouteSnapshot>::Ok(
                savorqt::db::SavorDbTasRouteService::FetchRoutes());
    });
    refresh_->setApply([this](const savorqt::db::TasRouteSnapshot& value,
        savorqt::gui::RefreshReason,
        const savorqt::gui::RefreshStatus&) { applySnapshot(value); });
    refresh_->setActive(false);

    connect(refreshButton, &QPushButton::clicked, this, [this]() { requestRefresh(); });
    connect(fit, &QPushButton::clicked, this, [this]() { view_->fitInView(scene_->itemsBoundingRect(), Qt::KeepAspectRatio); });
    connect(search_, &QLineEdit::textChanged, this, [this]() { if (snapshot_) applySnapshot(*snapshot_); });
    connect(rootFilter_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { if (snapshot_) applySnapshot(*snapshot_); });
    connect(scene_, &QGraphicsScene::selectionChanged, this, [this]() {
        const auto selected = scene_->selectedItems();
        if (!selected.empty()) selectNode(selected.front()->data(0).toLongLong());
    });
    connect(openVictories_, &QPushButton::clicked, this, [this]() { openSelected(); });
    routeView->activate = [this]() { openSelected(); };
    connect(saveLabel, &QPushButton::clicked, this, [this]() {
        if (selectedNodeId_ <= 0 || labelEdit_->text().trimmed().isEmpty()) return;
        std::string error;
        if (savorqt::db::SavorDbTasRouteService::RenameNode(
                selectedNodeId_, labelEdit_->text().trimmed().toStdString(), &error))
            requestRefresh();
        else
            detail_->setText(QString::fromStdString(error));
    });
}

void TasRoutesTab::setPageActive(bool active) {
    active_ = active;
    refresh_->setActive(active);
    if (active) requestRefresh();
}

void TasRoutesTab::requestRefresh() {
    if (active_) refresh_->requestRefresh(savorqt::gui::RefreshReason::Manual);
}

void TasRoutesTab::applySnapshot(const savorqt::db::TasRouteSnapshot& value) {
    if (!snapshot_) snapshot_ = new savorqt::db::TasRouteSnapshot;
    *snapshot_ = value;
    const auto oldCenter = view_->mapToScene(view_->viewport()->rect().center());
    scene_->clear();
    rootFilter_->blockSignals(true);
    const auto selectedRoot = rootFilter_->currentData().toLongLong();
    rootFilter_->clear();
    rootFilter_->addItem(QStringLiteral("All route roots"), 0);
    for (const auto& node : value.nodes)
        if (!node.parent_route_node_id)
            rootFilter_->addItem(QString::fromStdString(node.label), node.route_node_id);
    int restoreIndex = rootFilter_->findData(selectedRoot);
    rootFilter_->setCurrentIndex(std::max(0, restoreIndex));
    rootFilter_->blockSignals(false);

    std::unordered_map<std::int64_t, const savor::db::TasRouteNodeSnapshot*> byId;
    std::unordered_map<std::int64_t, int> depth;
    for (const auto& node : value.nodes) byId[node.route_node_id] = &node;
    const auto rootOf = [&](std::int64_t id) {
        while (byId.contains(id) && byId[id]->parent_route_node_id)
            id = *byId[id]->parent_route_node_id;
        return id;
    };
    std::function<int(std::int64_t)> depthOf = [&](std::int64_t id) {
        if (depth.contains(id)) return depth[id];
        const auto* node = byId[id];
        return depth[id] = node && node->parent_route_node_id ? depthOf(*node->parent_route_node_id) + 1 : 0;
    };
    std::map<int, int> rowsAtDepth;
    std::unordered_map<std::int64_t, QPointF> positions;
    std::unordered_map<std::int64_t, QGraphicsRectItem*> items;
    const QString needle = search_->text().trimmed();
    const auto rootFilter = rootFilter_->currentData().toLongLong();
    for (const auto& node : value.nodes) {
        if (rootFilter > 0 && rootOf(node.route_node_id) != rootFilter) continue;
        if (!needle.isEmpty() && !QString::fromStdString(node.label).contains(needle, Qt::CaseInsensitive)) continue;
        const int d = depthOf(node.route_node_id);
        const int row = rowsAtDepth[d]++;
        const QPointF position(d * 235.0, row * 105.0);
        positions[node.route_node_id] = position;
        const QRectF rect(position, QSizeF(190, 70));
        const bool activity = node.node_kind == savor::db::TasRouteNodeKind::Activity;
        QColor fill = activity ? QColor("#173d3a") : QColor("#342d1a");
        if (node.status == "FAILED" || node.status == "INTERRUPTED") fill = QColor("#4b2020");
        if (node.status == "PENDING" || node.status == "VALIDATING") fill = QColor("#3d3520");
        auto* item = scene_->addRect(rect, QPen(QColor("#769c91"), 2), QBrush(fill));
        item->setFlag(QGraphicsItem::ItemIsSelectable);
        item->setData(0, node.route_node_id);
        auto* text = scene_->addText(QString::fromStdString(node.label));
        text->setDefaultTextColor(Qt::white);
        text->setTextWidth(170);
        text->setPos(position + QPointF(10, 8));
        text->setParentItem(item);
        items[node.route_node_id] = item;
        if (node.route_node_id == selectedNodeId_) item->setSelected(true);
    }
    for (const auto& node : value.nodes) {
        if (!node.parent_route_node_id || !items.contains(node.route_node_id) ||
            !items.contains(*node.parent_route_node_id)) continue;
        const auto from = positions[*node.parent_route_node_id] + QPointF(190, 35);
        const auto to = positions[node.route_node_id] + QPointF(0, 35);
        scene_->addLine(QLineF(from, to), QPen(QColor("#6f817c"), 2))->setZValue(-1);
    }
    summary_->setText(QStringLiteral("%1 semantic nodes, %2 recorded Victory branches")
        .arg(value.nodes.size()).arg(value.branches.size()));
    if (!items.empty()) view_->centerOn(oldCenter);
    if (selectedNodeId_ > 0) selectNode(selectedNodeId_);
}

void TasRoutesTab::selectNode(std::int64_t id) {
    selectedNodeId_ = id;
    selectedIsBattle_ = false;
    if (!snapshot_) return;
    for (const auto& node : snapshot_->nodes) {
        if (node.route_node_id != id) continue;
        selectedIsBattle_ = node.node_kind == savor::db::TasRouteNodeKind::Activity && node.activity_kind == "battle";
        labelEdit_->setText(QString::fromStdString(node.label));
        detail_->setText(QStringLiteral("%1\nStatus: %2\n%3")
            .arg(node.node_kind == savor::db::TasRouteNodeKind::Activity ? QStringLiteral("Activity") : QStringLiteral("Checkpoint"))
            .arg(QString::fromStdString(node.status))
            .arg(QString::fromStdString(node.description)));
        break;
    }
    openVictories_->setEnabled(selectedIsBattle_);
}

void TasRoutesTab::openSelected() {
    if (selectedIsBattle_ && selectedNodeId_ > 0 && actions_.openVictoryResults)
        actions_.openVictoryResults(selectedNodeId_);
}
