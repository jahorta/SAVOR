#include "MainWindow.h"

#include <QAction>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QKeySequence>
#include <QMessageBox>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QToolBar>
#include <QUrl>
#include <QVariantMap>
#include <QStringList>
#include <QtConcurrent/QtConcurrentRun>

#include <filesystem>
#include <utility>

namespace savor::qt3d::gui {
namespace {

[[nodiscard]] QString normalizedAbsolutePath(const QString& path) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    return QDir::cleanPath(QFileInfo(trimmed).absoluteFilePath());
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    buildUi();
    statusBar()->showMessage("Ready. Open an MLD file to render.");
}

void MainWindow::buildUi() {
    auto* fileMenu = menuBar()->addMenu("&File");
    openAction_ = fileMenu->addAction("&Open MLD...");
    openAction_->setShortcut(QKeySequence::Open);
    connect(openAction_, &QAction::triggered, this, &MainWindow::chooseAndLoadMldFile);
    recentFilesMenu_ = fileMenu->addMenu("Recent Files");
    recentMldFiles_ = readRecentMldFiles();
    rebuildRecentFilesMenu();

    loadWatcher_ = new QFutureWatcher<savor::navigation::NavigationAreaLoadResult>(this);
    connect(loadWatcher_, &QFutureWatcher<savor::navigation::NavigationAreaLoadResult>::finished,
        this, &MainWindow::finishMldLoad);

    auto* viewToolbar = addToolBar("Layers");
    auto* debugMenu = menuBar()->addMenu("Debug");
    visibilityDebugAction_ = debugMenu->addAction("Visibility Snapshot Logging");
    visibilityDebugAction_->setCheckable(true);
    visibilityDebugAction_->setChecked(false);
    connect(visibilityDebugAction_, &QAction::toggled, this, [this](const bool checked) {
        visibilityDebugEnabled_ = checked;
        appendDiagnosticLine(QString("Debug visibility snapshot logging %1").arg(checked ? "ENABLED" : "DISABLED"));
    });

    groundsAction_ = viewToolbar->addAction("Ground");
    groundsAction_->setCheckable(true);
    groundsAction_->setChecked(true);
    connect(groundsAction_, &QAction::toggled, this, [this](const bool checked) {
        setLayerVisibility(VisibilityTreeWidget::LayerKind::Grounds, checked);
    });

    linksAction_ = viewToolbar->addAction("Links");
    linksAction_->setCheckable(true);
    linksAction_->setChecked(true);
    connect(linksAction_, &QAction::toggled, this, [this](const bool checked) {
        setLayerVisibility(VisibilityTreeWidget::LayerKind::Links, checked);
    });

    triggersAction_ = viewToolbar->addAction("Triggers");
    triggersAction_->setCheckable(true);
    triggersAction_->setChecked(true);
    connect(triggersAction_, &QAction::toggled, this, [this](const bool checked) {
        setLayerVisibility(VisibilityTreeWidget::LayerKind::Triggers, checked);
    });

    collisionsAction_ = viewToolbar->addAction("Collision");
    collisionsAction_->setCheckable(true);
    collisionsAction_->setChecked(true);
    connect(collisionsAction_, &QAction::toggled, this, [this](const bool checked) {
        setLayerVisibility(VisibilityTreeWidget::LayerKind::Collisions, checked);
    });

    movingObjectsAction_ = viewToolbar->addAction("MovingObjects");
    movingObjectsAction_->setCheckable(true);
    movingObjectsAction_->setChecked(true);
    connect(movingObjectsAction_, &QAction::toggled, this, [this](const bool checked) {
        setLayerVisibility(VisibilityTreeWidget::LayerKind::MovingObjects, checked);
    });

    unknownsAction_ = viewToolbar->addAction("Unknown");
    unknownsAction_->setCheckable(true);
    unknownsAction_->setChecked(true);
    connect(unknownsAction_, &QAction::toggled, this, [this](const bool checked) {
        setLayerVisibility(VisibilityTreeWidget::LayerKind::Unknowns, checked);
    });

    visibilityWidget_ = new VisibilityTreeWidget(this);
    auto* visibilityDock = new QDockWidget("Parsing Controls", this);
    visibilityDock->setWidget(visibilityWidget_);
    visibilityDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, visibilityDock);

    connect(visibilityWidget_, &VisibilityTreeWidget::allToggled, this, [this](const bool visible) {
        setAllVisibility(visible);
    });
    connect(visibilityWidget_, &VisibilityTreeWidget::layerToggled, this, [this](const int layer, const bool visible) {
        setLayerVisibility(static_cast<VisibilityTreeWidget::LayerKind>(layer), visible);
    });
    connect(visibilityWidget_, &VisibilityTreeWidget::leafToggled, this, [this](const int layer, const int index, const bool visible) {
        setLeafVisibility(static_cast<VisibilityTreeWidget::LayerKind>(layer), index, visible);
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
    quickView_->setSource(QUrl(QStringLiteral("qrc:/qml/Qml/ViewerScene.qml")));
    setCentralWidget(quickView_);

    setWindowTitle("SavorQt3D - Quick 3D Viewer");
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
    root->setProperty("showMovingObjects", showMovingObjects_);
    root->setProperty("showUnknowns", showUnknowns_);
}

void MainWindow::chooseAndLoadMldFile() {
    const QString path = QFileDialog::getOpenFileName(this,
        "Open MLD",
        readLastMldDirectory(),
        "Skies MLD Files (*.mld *.MLD);;All Files (*.*)");
    if (path.isEmpty()) {
        return;
    }

    startMldLoad(path);
}

void MainWindow::startMldLoad(const QString& path) {
    if (loadWatcher_ == nullptr || loadWatcher_->isRunning()) {
        return;
    }

    diagnosticsView_->clear();
    appendDiagnosticLine(QString("Loading: %1").arg(path));
    appendDiagnosticLine("Parsing and adapting in a background worker...");
    pendingLoadPath_ = path;
    storeLastMldDirectory(path);
    openAction_->setEnabled(false);
    recentFilesMenu_->setEnabled(false);
    statusBar()->showMessage(QString("Loading %1...").arg(QFileInfo(path).fileName()));

    const std::wstring nativePath = path.toStdWString();
    loadWatcher_->setFuture(QtConcurrent::run([nativePath]() {
        savor::navigation::NavigationAreaLoader loader{};
        return loader.loadFile(std::filesystem::path(nativePath));
    }));
}

void MainWindow::finishMldLoad() {
    if (loadWatcher_ == nullptr) {
        return;
    }

    openAction_->setEnabled(true);
    auto result = loadWatcher_->result();

    if (!result.hasModel()) {
        rebuildRecentFilesMenu();
        appendLoadDiagnostics(result.diagnostics);
        statusBar()->showMessage(QString("Failed to load %1").arg(QFileInfo(pendingLoadPath_).fileName()));
        QMessageBox::warning(this,
            "Load failed",
            "Could not parse the selected MLD. See Diagnostics for details.");
        pendingLoadPath_.clear();
        return;
    }

    recordRecentMldFile(pendingLoadPath_);
    currentModel_ = std::move(*result.model);
    auto runtimeScene = runtimeSceneConverter_.convert(*currentModel_);
    applyRuntimeScene(std::move(runtimeScene));

    const QString fileName = QFileInfo(pendingLoadPath_).fileName();
    if (result.status == savor::navigation::NavigationAreaLoadStatus::Partial) {
        appendDiagnosticLine("WARNING: Ground geometry is incomplete; pathfinding must remain disabled for this model.");
        statusBar()->showMessage(QString("Loaded partial model from %1 (pathfinding unavailable)").arg(fileName));
    } else {
        statusBar()->showMessage(QString("Loaded complete model from %1").arg(fileName));
    }
    pendingLoadPath_.clear();
}

void MainWindow::appendLoadDiagnostics(
    const std::vector<savor::navigation::NavigationDiagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        QString severity = QStringLiteral("INFO");
        if (diagnostic.severity == savor::navigation::NavigationDiagnosticSeverity::Warning) {
            severity = QStringLiteral("WARNING");
        } else if (diagnostic.severity == savor::navigation::NavigationDiagnosticSeverity::Error) {
            severity = QStringLiteral("ERROR");
        }
        appendDiagnosticLine(QStringLiteral("%1: %2")
            .arg(severity, QString::fromStdString(diagnostic.message)));
    }
}

QString MainWindow::readLastMldDirectory() const {
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    QString directory = settings.value(kLastMldDirectoryKey).toString();
    if (directory.isEmpty()) {
        const QString legacyPath = settings.value(kLegacyLastMldPathKey).toString();
        if (!legacyPath.isEmpty()) {
            directory = QFileInfo(legacyPath).absolutePath();
        }
    }
    settings.endGroup();
    return directory;
}

void MainWindow::storeLastMldDirectory(const QString& path) const {
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    settings.setValue(kLastMldDirectoryKey, QFileInfo(path).absolutePath());
    settings.endGroup();
}

QStringList MainWindow::readRecentMldFiles() const {
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    const QStringList storedPaths = settings.value(kRecentMldFilesKey).toStringList();
    settings.endGroup();

    QStringList recentFiles{};
    for (const QString& storedPath : storedPaths) {
        const QString normalized = normalizedAbsolutePath(storedPath);
        if (normalized.isEmpty()) {
            continue;
        }

        bool duplicate = false;
        for (const QString& existing : recentFiles) {
            if (existing.compare(normalized, Qt::CaseInsensitive) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            recentFiles.push_back(normalized);
        }
        if (recentFiles.size() == kMaxRecentMldFiles) {
            break;
        }
    }
    return recentFiles;
}

void MainWindow::recordRecentMldFile(const QString& path) {
    const QString normalized = normalizedAbsolutePath(path);
    if (normalized.isEmpty()) {
        return;
    }

    recentMldFiles_.removeIf([&normalized](const QString& existing) {
        return existing.compare(normalized, Qt::CaseInsensitive) == 0;
    });
    recentMldFiles_.prepend(normalized);
    while (recentMldFiles_.size() > kMaxRecentMldFiles) {
        recentMldFiles_.removeLast();
    }

    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    settings.setValue(kRecentMldFilesKey, recentMldFiles_);
    settings.endGroup();
    rebuildRecentFilesMenu();
}

void MainWindow::rebuildRecentFilesMenu() {
    if (recentFilesMenu_ == nullptr) {
        return;
    }

    recentFilesMenu_->clear();
    if (recentMldFiles_.isEmpty()) {
        QAction* emptyAction = recentFilesMenu_->addAction("(No Recent Files)");
        emptyAction->setEnabled(false);
        recentFilesMenu_->setEnabled(false);
        return;
    }

    for (int index = 0; index < recentMldFiles_.size(); ++index) {
        const QString path = recentMldFiles_.at(index);
        QString menuPath = QDir::toNativeSeparators(path);
        menuPath.replace('&', "&&");
        QAction* action = recentFilesMenu_->addAction(QStringLiteral("&%1 %2")
            .arg(index + 1)
            .arg(menuPath));
        action->setData(path);
        action->setStatusTip(path);
        connect(action, &QAction::triggered, this, [this, path]() {
            startMldLoad(path);
        });
    }
    recentFilesMenu_->setEnabled(true);
}

void MainWindow::applyRuntimeScene(RuntimeSceneData data) {
    if (quickView_ == nullptr || quickView_->rootObject() == nullptr) {
        appendDiagnosticLine("Viewer is not ready yet; deferring scene assignment.");
        pendingRuntimeScene_ = std::move(data);
        return;
    }

    auto previousGeometry = std::move(geometryStore_);
    geometryStore_ = std::move(data.geometries);
    appendDiagnosticLine("Assigning objects to layers.");
    groundMeshes_ = std::move(data.grounds);
    linkMeshes_ = std::move(data.links);
    collisionMeshes_ = std::move(data.collisions);
    triggerMeshes_ = std::move(data.triggers);
    movingObjectMeshes_ = std::move(data.movingObjects);
    unknownMeshes_ = std::move(data.unknowns);

    auto applyDefaultVisibility = [](QVariantList& list) {
        for (int i = 0; i < list.size(); ++i) {
            QVariantMap map = list.at(i).toMap();
            if (!map.contains("visible")) {
                map.insert("visible", true);
            }
            list[i] = map;
        }
    };
    applyDefaultVisibility(groundMeshes_);
    applyDefaultVisibility(linkMeshes_);
    applyDefaultVisibility(collisionMeshes_);
    applyDefaultVisibility(triggerMeshes_);
    applyDefaultVisibility(movingObjectMeshes_);
    applyDefaultVisibility(unknownMeshes_);

    setAllVisibility(true);

    QObject* root = quickView_->rootObject();
    root->setProperty("cameraTarget", QVariant::fromValue(data.center));
    root->setProperty("cameraDistance", data.extent * 1.75f);

    for (const auto& line : data.diagnostics) {
        appendDiagnosticLine(QString::fromStdString(line));
    }

    statusBar()->showMessage(QString("Scene updated: Grounds=%1, Links=%2, Collisions=%3, Triggers=%4, MovingObjects=%5, Unknown=%6")
        .arg(static_cast<int>(groundMeshes_.size()))
        .arg(static_cast<int>(linkMeshes_.size()))
        .arg(static_cast<int>(collisionMeshes_.size()))
        .arg(static_cast<int>(triggerMeshes_.size()))
        .arg(static_cast<int>(movingObjectMeshes_.size()))
        .arg(static_cast<int>(unknownMeshes_.size())));
}

void MainWindow::appendDiagnosticLine(const QString& line) {
    if (diagnosticsView_ != nullptr) {
        diagnosticsView_->appendPlainText(line);
    }
}

void MainWindow::applyMeshesToQml() {
    if (quickView_ == nullptr || quickView_->rootObject() == nullptr) {
        return;
    }

    if (visibilityDebugEnabled_) {
        logVisibilitySnapshot(QStringLiteral("applyMeshesToQml before root property update"));
    }

    QObject* root = quickView_->rootObject();
    root->setProperty("groundMeshes", groundMeshes_);
    root->setProperty("linkMeshes", linkMeshes_);
    root->setProperty("collisionMeshes", collisionMeshes_);
    root->setProperty("triggerMeshes", triggerMeshes_);
    root->setProperty("movingObjectMeshes", movingObjectMeshes_);
    root->setProperty("unknownMeshes", unknownMeshes_);

    auto hasVisible = [](const QVariantList& meshes) {
        for (const auto& entry : meshes) {
            if (entry.toMap().value("visible", true).toBool()) {
                return true;
            }
        }
        return false;
    };
    showGrounds_ = hasVisible(groundMeshes_);
    showLinks_ = hasVisible(linkMeshes_);
    showCollisions_ = hasVisible(collisionMeshes_);
    showTriggers_ = hasVisible(triggerMeshes_);
    showMovingObjects_ = hasVisible(movingObjectMeshes_);
    showUnknowns_ = hasVisible(unknownMeshes_);

    setActionCheckedNoSignal(groundsAction_, showGrounds_);
    setActionCheckedNoSignal(linksAction_, showLinks_);
    setActionCheckedNoSignal(collisionsAction_, showCollisions_);
    setActionCheckedNoSignal(triggersAction_, showTriggers_);
    setActionCheckedNoSignal(movingObjectsAction_, showMovingObjects_);
    setActionCheckedNoSignal(unknownsAction_, showUnknowns_);

    syncLayerPropertiesToQml();
}

void MainWindow::setLayerVisibility(const VisibilityTreeWidget::LayerKind layer, const bool visible) {
    QVariantList* meshes = meshesForLayer(layer);
    if (meshes == nullptr) {
        return;
    }

    for (int i = 0; i < meshes->size(); ++i) {
        QVariantMap map = meshes->at(i).toMap();
        map.insert("visible", visible);
        (*meshes)[i] = map;
    }

    visibilityWidget_->setLayers(
        groundMeshes_, linkMeshes_, collisionMeshes_, triggerMeshes_, movingObjectMeshes_, unknownMeshes_);
    applyMeshesToQml();
}

void MainWindow::setAllVisibility(const bool visible) {
    auto setMeshes = [visible](QVariantList& meshes) {
        for (int i = 0; i < meshes.size(); ++i) {
            QVariantMap map = meshes.at(i).toMap();
            map.insert("visible", visible);
            meshes[i] = map;
        }
    };
    setMeshes(groundMeshes_);
    setMeshes(linkMeshes_);
    setMeshes(collisionMeshes_);
    setMeshes(triggerMeshes_);
    setMeshes(movingObjectMeshes_);
    setMeshes(unknownMeshes_);

    visibilityWidget_->setLayers(
        groundMeshes_, linkMeshes_, collisionMeshes_, triggerMeshes_, movingObjectMeshes_, unknownMeshes_);
    applyMeshesToQml();
}

QVariantList* MainWindow::meshesForLayer(const VisibilityTreeWidget::LayerKind layer) {
    switch (layer) {
    case VisibilityTreeWidget::LayerKind::Grounds:
        return &groundMeshes_;
    case VisibilityTreeWidget::LayerKind::Links:
        return &linkMeshes_;
    case VisibilityTreeWidget::LayerKind::Collisions:
        return &collisionMeshes_;
    case VisibilityTreeWidget::LayerKind::Triggers:
        return &triggerMeshes_;
    case VisibilityTreeWidget::LayerKind::MovingObjects:
        return &movingObjectMeshes_;
    case VisibilityTreeWidget::LayerKind::Unknowns:
        return &unknownMeshes_;
    default:
        return nullptr;
    }
}

void MainWindow::setLeafVisibility(const VisibilityTreeWidget::LayerKind layer, const int index, const bool visible) {
    QVariantList* meshes = meshesForLayer(layer);
    if (meshes == nullptr || index < 0 || index >= meshes->size()) {
        return;
    }

    QVariantMap map = meshes->at(index).toMap();
    map.insert("visible", visible);
    (*meshes)[index] = map;

    if (visibilityDebugEnabled_) {
        logVisibilitySnapshot(QString("setLeafVisibility layer=%1 index=%2 visible=%3")
            .arg(static_cast<int>(layer))
            .arg(index)
            .arg(visible ? "true" : "false"));
    }

    applyMeshesToQml();
}

void MainWindow::logVisibilitySnapshot(const QString& reason) {
    auto summarizeLayer = [](const QString& name, const QVariantList& meshes) {
        int falseCount = 0;
        QStringList falseLabels{};
        for (int i = 0; i < meshes.size(); ++i) {
            const QVariantMap map = meshes.at(i).toMap();
            if (!map.value("visible", true).toBool()) {
                ++falseCount;
                const QString label = map.value("label").toString();
                falseLabels.push_back(QString("%1[%2]").arg(label.isEmpty() ? QStringLiteral("(unnamed)") : label).arg(i));
            }
        }

        return QString("%1 total=%2 hidden=%3 hiddenItems=%4")
            .arg(name)
            .arg(meshes.size())
            .arg(falseCount)
            .arg(falseLabels.isEmpty() ? QStringLiteral("none") : falseLabels.join(QStringLiteral(", ")));
    };

    appendDiagnosticLine(QString("[VisibilityDebug] %1").arg(reason));
    appendDiagnosticLine(QString("[VisibilityDebug] %1").arg(summarizeLayer(QStringLiteral("Grounds"), groundMeshes_)));
    appendDiagnosticLine(QString("[VisibilityDebug] %1").arg(summarizeLayer(QStringLiteral("Links"), linkMeshes_)));
    appendDiagnosticLine(QString("[VisibilityDebug] %1").arg(summarizeLayer(QStringLiteral("Collisions"), collisionMeshes_)));
    appendDiagnosticLine(QString("[VisibilityDebug] %1").arg(summarizeLayer(QStringLiteral("Triggers"), triggerMeshes_)));
    appendDiagnosticLine(QString("[VisibilityDebug] %1").arg(summarizeLayer(QStringLiteral("MovingObjects"), movingObjectMeshes_)));
    appendDiagnosticLine(QString("[VisibilityDebug] %1").arg(summarizeLayer(QStringLiteral("Unknowns"), unknownMeshes_)));
}

void MainWindow::setActionCheckedNoSignal(QAction* action, const bool checked) {
    if (action == nullptr) {
        return;
    }
    const QSignalBlocker blocker(action);
    action->setChecked(checked);
}

} // namespace savor::qt3d::gui
