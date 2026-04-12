#include "MainWindow.h"

#include "../../SoaSimMLD/Parsing/GeometryBuilder.h"

#include <QAction>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
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

#include <cstdint>
#include <span>
#include <utility>

namespace soasim::qt3d::gui {

MainWindow::MainWindow(const scene::IQtSceneBuilder& sceneBuilder, QWidget* parent)
    : QMainWindow(parent)
    , sceneBuilder_(sceneBuilder) {
    buildUi();
    statusBar()->showMessage("Ready. Open an MLD file to render.");
    tryLoadLastMldOnStartup();
}

void MainWindow::buildUi() {
    auto* fileToolbar = addToolBar("File");
    auto* openAction = fileToolbar->addAction("Open MLD...");
    connect(openAction, &QAction::triggered, this, &MainWindow::chooseAndLoadMldFile);

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
        readLastMldPath(),
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
    storeLastMldPath(path);
    statusBar()->showMessage(QString("Loaded %1").arg(info.fileName()));
    return true;
}

void MainWindow::tryLoadLastMldOnStartup() {
    const QString lastPath = readLastMldPath();
    if (lastPath.isEmpty()) {
        return;
    }

    const QFileInfo fileInfo(lastPath);
    if (!fileInfo.exists()) {
        appendDiagnosticLine(QString("Startup: saved MLD file not found: %1").arg(lastPath));
        return;
    }

    if (!loadMldFile(lastPath)) {
        appendDiagnosticLine(QString("Startup: failed to load saved MLD file: %1").arg(lastPath));
    }
}

QString MainWindow::readLastMldPath() const {
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    const QString path = settings.value(kLastMldPathKey).toString();
    settings.endGroup();
    return path;
}

void MainWindow::storeLastMldPath(const QString& path) const {
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    settings.setValue(kLastMldPathKey, path);
    settings.endGroup();
}

void MainWindow::applyRuntimeScene(RuntimeSceneData data) {
    if (quickView_ == nullptr || quickView_->rootObject() == nullptr) {
        appendDiagnosticLine("Viewer is not ready yet; deferring scene assignment.");
        pendingRuntimeScene_ = std::move(data);
        return;
    }

    appendDiagnosticLine(QString("Assigning objects to layers."));
    groundMeshes_ = std::move(data.grounds);
    linkMeshes_ = std::move(data.links);
    collisionMeshes_ = std::move(data.collisions);
    triggerMeshes_ = std::move(data.triggers);
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
    applyDefaultVisibility(unknownMeshes_);

    setAllVisibility(true);

    QObject* root = quickView_->rootObject();
    root->setProperty("cameraTarget", QVariant::fromValue(data.center));
    root->setProperty("cameraDistance", data.extent * 1.75f);

    for (const auto& line : data.diagnostics) {
        appendDiagnosticLine(QString::fromStdString(line));
    }

    statusBar()->showMessage(QString("Scene updated: Grounds=%1, Links=%2, Collisions=%3, Triggers=%4, Unknown=%5")
        .arg(static_cast<int>(groundMeshes_.size()))
        .arg(static_cast<int>(linkMeshes_.size()))
        .arg(static_cast<int>(collisionMeshes_.size()))
        .arg(static_cast<int>(triggerMeshes_.size()))
        .arg(static_cast<int>(unknownMeshes_.size())));
}

void MainWindow::appendDiagnosticLine(const QString& line) {
    diagnosticsView_->appendPlainText(line);
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
    showUnknowns_ = hasVisible(unknownMeshes_);

    setActionCheckedNoSignal(groundsAction_, showGrounds_);
    setActionCheckedNoSignal(linksAction_, showLinks_);
    setActionCheckedNoSignal(collisionsAction_, showCollisions_);
    setActionCheckedNoSignal(triggersAction_, showTriggers_);
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

    visibilityWidget_->setLayers(groundMeshes_, linkMeshes_, collisionMeshes_, triggerMeshes_, unknownMeshes_);
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
    setMeshes(unknownMeshes_);

    visibilityWidget_->setLayers(groundMeshes_, linkMeshes_, collisionMeshes_, triggerMeshes_, unknownMeshes_);
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
    appendDiagnosticLine(QString("[VisibilityDebug] %1").arg(summarizeLayer(QStringLiteral("Unknowns"), unknownMeshes_)));
}

void MainWindow::setActionCheckedNoSignal(QAction* action, const bool checked) {
    if (action == nullptr) {
        return;
    }
    const QSignalBlocker blocker(action);
    action->setChecked(checked);
}

} // namespace soasim::qt3d::gui
