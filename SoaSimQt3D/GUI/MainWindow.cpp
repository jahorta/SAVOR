#include "MainWindow.h"

#include "../../SoaSimMLD/Parsing/GeometryBuilder.h"

#include <QAction>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QQuickItem>
#include <QPlainTextEdit>
#include <QQmlError>
#include <QQuickWidget>
#include <QStatusBar>
#include <QToolBar>
#include <QUrl>
#include <QQmlContext>

#include <utility>
#include <cstdint>
#include <span>

namespace soasim::qt3d::gui {

MainWindow::MainWindow(const scene::IQtSceneBuilder& sceneBuilder, QWidget* parent)
    : QMainWindow(parent)
    , sceneBuilder_(sceneBuilder) {
    buildUi();
    statusBar()->showMessage("Ready. Open an MLD file to render.");
}

void MainWindow::buildUi() {
    auto* fileToolbar = addToolBar("File");
    auto* openAction = fileToolbar->addAction("Open MLD...");
    connect(openAction, &QAction::triggered, this, &MainWindow::chooseAndLoadMldFile);

    auto* viewToolbar = addToolBar("Layers");
    auto* groundsAction = viewToolbar->addAction("Ground");
    groundsAction->setCheckable(true);
    groundsAction->setChecked(true);
    connect(groundsAction, &QAction::toggled, this, [this](const bool checked) {
        showGrounds_ = checked;
        syncLayerPropertiesToQml();
    });

    auto* linksAction = viewToolbar->addAction("Links");
    linksAction->setCheckable(true);
    linksAction->setChecked(true);
    connect(linksAction, &QAction::toggled, this, [this](const bool checked) {
        showLinks_ = checked;
        syncLayerPropertiesToQml();
    });

    auto* triggerAction = viewToolbar->addAction("Triggers");
    triggerAction->setCheckable(true);
    triggerAction->setChecked(true);
    connect(triggerAction, &QAction::toggled, this, [this](const bool checked) {
        showTriggers_ = checked;
        syncLayerPropertiesToQml();
    });

    auto* collisionAction = viewToolbar->addAction("Collision");
    collisionAction->setCheckable(true);
    collisionAction->setChecked(true);
    connect(collisionAction, &QAction::toggled, this, [this](const bool checked) {
        showCollisions_ = checked;
        syncLayerPropertiesToQml();
    });

    auto* unknownAction = viewToolbar->addAction("Unknown");
    unknownAction->setCheckable(true);
    unknownAction->setChecked(true);
    connect(unknownAction, &QAction::toggled, this, [this](const bool checked) {
        showUnknowns_ = checked;
        syncLayerPropertiesToQml();
    });

    diagnosticsView_ = new QPlainTextEdit(this);
    diagnosticsView_->setReadOnly(true);
    auto* diagnosticsDock = new QDockWidget("Diagnostics", this);
    diagnosticsDock->setWidget(diagnosticsView_);
    diagnosticsDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, diagnosticsDock);

    quickView_ = new QQuickWidget(this);
    quickView_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    quickView_->rootContext()->setContextProperty("viewerWindow", this);
    connect(quickView_, &QQuickWidget::statusChanged, this, &MainWindow::handleQuickViewStatusChanged);
    quickView_->setSource(QUrl(QStringLiteral("qrc:/qml/ViewerScene.qml")));
    setCentralWidget(quickView_);

    syncLayerPropertiesToQml();

    setWindowTitle("SoaSimQt3D - Quick 3D Viewer");
    resize(1280, 820);
}

void MainWindow::handleQuickViewStatusChanged(const QQuickWidget::Status status) {
    if (quickView_ == nullptr) {
        return;
    }

    if (status == QQuickWidget::Error) {
        appendDiagnosticLine("ERROR: ViewerScene.qml failed to load.");
        const QList<QQmlError> errors = quickView_->errors();
        for (const QQmlError& error : errors) {
            appendDiagnosticLine(error.toString());
        }
        return;
    }

    if (status != QQuickWidget::Ready || quickView_->rootObject() == nullptr) {
        return;
    }

    syncLayerPropertiesToQml();
    if (pendingRuntimeScene_.has_value()) {
        RuntimeSceneData pending = std::move(*pendingRuntimeScene_);
        pendingRuntimeScene_.reset();
        applyRuntimeScene(std::move(pending));
    }
}

void MainWindow::syncLayerPropertiesToQml() {
    if (quickView_ == nullptr || quickView_->rootObject() == nullptr) {
        return;
    }

    QObject* root = quickView_->rootObject();
    root->setProperty("showGrounds", showGrounds_);
    root->setProperty("showLinks", showLinks_);
    root->setProperty("showCollisions", showCollisions_);
    root->setProperty("showTriggers", showTriggers_);
    root->setProperty("showUnknowns", showUnknowns_);
}

void MainWindow::chooseAndLoadMldFile() {
    const QString path = QFileDialog::getOpenFileName(this,
        "Open MLD",
        QString(),
        "Skies MLD Files (*.mld *.MLD);;All Files (*.*)");
    if (path.isEmpty()) {
        return;
    }

    if (!loadMldFile(path)) {
        QMessageBox::warning(this,
            "Load failed",
            "Could not parse/render the selected MLD. See Diagnostics pane for details.");
    }
}

bool MainWindow::loadMldFile(const QString& path) {
    diagnosticsView_->clear();
    appendDiagnosticLine(QString("Loading: %1").arg(path));
    geometryStore_.clear();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        appendDiagnosticLine(QString("ERROR: Failed to open file: %1").arg(path));
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty()) {
        appendDiagnosticLine("ERROR: Selected file is empty.");
        return false;
    }


    appendDiagnosticLine(QString("Parsing MLD File."));
    soasim::mld::parsing::MldParser parser{};
    soasim::mld::parsing::ParseOptions options{};
    options.preserveUnknownEntries = true;
    const auto parse = parser.parse(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
            static_cast<std::size_t>(bytes.size())),
        options);

    appendDiagnosticLine(QString("Building Geometry."));
    soasim::mld::parsing::GeometryBuilder geometryBuilder{};
    const auto geometry = geometryBuilder.build(parse);

    appendDiagnosticLine(QString("Building Scene"));
    const auto scene = sceneBuilder_.buildScene(geometry);

    appendDiagnosticLine(QString("Converting to Runtime Scene"));
    auto runtimeScene = runtimeSceneConverter_.convert(parse, scene);
    geometryStore_ = std::move(runtimeScene.geometries);
    applyRuntimeScene(std::move(runtimeScene));

    const QFileInfo info(path);
    statusBar()->showMessage(QString("Loaded %1").arg(info.fileName()));
    return true;
}

void MainWindow::applyRuntimeScene(RuntimeSceneData data) {
    if (quickView_ == nullptr || quickView_->rootObject() == nullptr) {
        appendDiagnosticLine("Viewer is not ready yet; deferring scene assignment.");
        pendingRuntimeScene_ = std::move(data);
        return;
    }

    appendDiagnosticLine(QString("Assigning objects to layers."));
    QObject* root = quickView_->rootObject();
    root->setProperty("groundMeshes", data.grounds);
    root->setProperty("linkMeshes", data.links);
    root->setProperty("collisionMeshes", data.collisions);
    root->setProperty("triggerMeshes", data.triggers);
    root->setProperty("unknownMeshes", data.unknowns);
    root->setProperty("cameraTarget", QVariant::fromValue(data.center));
    root->setProperty("cameraDistance", data.extent * 1.75f);

    syncLayerPropertiesToQml();

    for (const auto& line : data.diagnostics) {
        appendDiagnosticLine(QString::fromStdString(line));
    }

    statusBar()->showMessage(QString("Scene updated: Grounds=%1, Links=%2, Collisions=%3, Triggers=%4, Unknown=%5")
        .arg(static_cast<int>(data.grounds.size()))
        .arg(static_cast<int>(data.links.size()))
        .arg(static_cast<int>(data.collisions.size()))
        .arg(static_cast<int>(data.triggers.size()))
        .arg(static_cast<int>(data.unknowns.size())));
}

void MainWindow::appendDiagnosticLine(const QString& line) {
    diagnosticsView_->appendPlainText(line);
}

} // namespace soasim::qt3d::gui
