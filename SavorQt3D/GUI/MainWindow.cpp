#include "MainWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QAbstractButton>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStandardItemModel>
#include <QToolBar>
#include <QUrl>
#include <QVariantMap>
#include <QStringList>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace savor::qt3d::gui {
namespace {

constexpr int kStartOptionIdRole = Qt::UserRole + 1;

[[nodiscard]] QString normalizedAbsolutePath(const QString& path) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    return QDir::cleanPath(QFileInfo(trimmed).absoluteFilePath());
}

[[nodiscard]] QString scriptAssociationLabel(
    const savor::navigation::NavigationScriptAssociationStatus status) {
    using Status = savor::navigation::NavigationScriptAssociationStatus;
    switch (status) {
    case Status::Missing:
        return QStringLiteral("missing");
    case Status::Matched:
        return QStringLiteral("matched");
    case Status::Rejected:
        return QStringLiteral("rejected");
    case Status::Unavailable:
    default:
        return QStringLiteral("unavailable");
    }
}

[[nodiscard]] QString scriptLoadLabel(const savor::navigation::NavigationScriptLoadStatus status) {
    using Status = savor::navigation::NavigationScriptLoadStatus;
    switch (status) {
    case Status::Complete:
        return QStringLiteral("complete");
    case Status::Partial:
        return QStringLiteral("partial");
    case Status::Failed:
        return QStringLiteral("failed");
    case Status::NotAttempted:
    default:
        return QStringLiteral("not attempted");
    }
}

[[nodiscard]] QString startKindLabel(const savor::navigation::NavigationStartKind kind) {
    using Kind = savor::navigation::NavigationStartKind;
    return kind == Kind::AuthoredArrival
        ? QStringLiteral("Authored arrival")
        : QStringLiteral("Scripted reposition");
}

[[nodiscard]] QString startOptionToolTip(
    const savor::navigation::NavigationStartOption& option) {
    QStringList lines{};
    lines.push_back(startKindLabel(option.kind));
    lines.push_back(option.availability == savor::navigation::NavigationStartAvailability::Resolvable
        ? QStringLiteral("Availability: resolvable")
        : QStringLiteral("Availability: incomplete (%1)")
              .arg(QString::fromStdString(option.unavailableReason)));
    if (option.groundTblId.has_value()) {
        lines.push_back(QStringLiteral("Ground tblId: %1").arg(*option.groundTblId));
    }
    if (option.position.has_value()) {
        lines.push_back(QStringLiteral("Position: (%1, %2, %3)")
            .arg(option.position->x, 0, 'g', 8)
            .arg(option.position->y, 0, 'g', 8)
            .arg(option.position->z, 0, 'g', 8));
    }
    lines.push_back(option.yawDegrees.has_value()
        ? QStringLiteral("Yaw: %1").arg(*option.yawDegrees, 0, 'g', 8)
        : QStringLiteral("Yaw: unknown"));

    for (const auto& variant : option.variants) {
        lines.push_back(QStringLiteral(""));
        lines.push_back(QStringLiteral("%1 @ 0x%2 (payload 0x%3)")
            .arg(QString::fromStdString(variant.source.sectionName))
            .arg(variant.source.instructionOffset, 0, 16)
            .arg(variant.source.instructionPayloadOffset, 0, 16));
        if (!variant.conditionSummary.empty()) {
            lines.push_back(QStringLiteral("Conditions: %1")
                .arg(QString::fromStdString(variant.conditionSummary)));
        }
        if (!variant.callPathSummary.empty()) {
            lines.push_back(QStringLiteral("Call path: %1")
                .arg(QString::fromStdString(variant.callPathSummary)));
        }
        if (!variant.conditionAnalysisComplete) {
            lines.push_back(QStringLiteral("Condition analysis includes opaque or incomplete clauses."));
        }
    }
    return lines.join('\n');
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
    loadRelatedSctAction_ = fileMenu->addAction("Load Related &SCT...");
    loadRelatedSctAction_->setEnabled(false);
    connect(loadRelatedSctAction_, &QAction::triggered,
        this, &MainWindow::chooseAndLoadRelatedSctFile);
    fileMenu->addSeparator();
    recentFilesMenu_ = fileMenu->addMenu("Recent Files");
    recentMldFiles_ = readRecentMldFiles();
    rebuildRecentFilesMenu();

    loadWatcher_ = new QFutureWatcher<savor::navigation::NavigationScenarioLoadResult>(this);
    connect(loadWatcher_, &QFutureWatcher<savor::navigation::NavigationScenarioLoadResult>::finished,
        this, &MainWindow::finishMldLoad);
    scriptLoadWatcher_ = new QFutureWatcher<savor::navigation::NavigationScriptLoadResult>(this);
    connect(scriptLoadWatcher_, &QFutureWatcher<savor::navigation::NavigationScriptLoadResult>::finished,
        this, &MainWindow::finishRelatedSctLoad);

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

    routeAction_ = viewToolbar->addAction("Route");
    routeAction_->setCheckable(true);
    routeAction_->setChecked(true);
    connect(routeAction_, &QAction::toggled, this, [this](const bool checked) {
        setLayerVisibility(VisibilityTreeWidget::LayerKind::Route, checked);
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

    auto* pathToolbar = addToolBar("Path");
    pathToolbar->addWidget(new QLabel(QStringLiteral("Start:"), pathToolbar));
    startSelector_ = new QComboBox(pathToolbar);
    startSelector_->setMinimumContentsLength(24);
    startSelector_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    startSelector_->setModel(new QStandardItemModel(startSelector_));
    startSelector_->setEnabled(false);
    pathToolbar->addWidget(startSelector_);
    rebuildStartSelector();
    connect(startSelector_, &QComboBox::currentIndexChanged,
        this, &MainWindow::handleStartSelectorChanged);
    pathToolbar->addWidget(new QLabel(QStringLiteral("Manual facing:"), pathToolbar));
    manualStartFacingSpinBox_ = new QDoubleSpinBox(pathToolbar);
    manualStartFacingSpinBox_->setRange(-180.0, 180.0);
    manualStartFacingSpinBox_->setDecimals(1);
    manualStartFacingSpinBox_->setSingleStep(5.0);
    manualStartFacingSpinBox_->setSuffix(QStringLiteral("\u00b0"));
    manualStartFacingSpinBox_->setWrapping(true);
    manualStartFacingSpinBox_->setToolTip(QStringLiteral(
        "Facing used by the next manually picked start. Editing it also rotates an existing manual start."));
    manualStartFacingSpinBox_->setEnabled(false);
    pathToolbar->addWidget(manualStartFacingSpinBox_);
    connect(manualStartFacingSpinBox_, &QDoubleSpinBox::valueChanged, this, [this](const double value) {
        manualStartYawDegrees_ = static_cast<float>(value);
        if (startAnchor_.has_value() && selectedStartOptionId_.isEmpty()) {
            startFacingYawDegrees_ = manualStartYawDegrees_;
            renderRouteOverlay(false);
        }
    });
    pathToolbar->addSeparator();
    endpointActionGroup_ = new QActionGroup(this);
    endpointActionGroup_->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
    setStartAction_ = pathToolbar->addAction("Set Start");
    setStartAction_->setCheckable(true);
    setStartAction_->setEnabled(false);
    endpointActionGroup_->addAction(setStartAction_);
    setGoalAction_ = pathToolbar->addAction("Set Goal");
    setGoalAction_->setCheckable(true);
    setGoalAction_->setEnabled(false);
    setGoalAction_->setToolTip(QStringLiteral("Pick a walkable ground surface or a projected trigger mesh."));
    endpointActionGroup_->addAction(setGoalAction_);
    connect(endpointActionGroup_, &QActionGroup::triggered, this, [this](QAction*) {
        syncEndpointModeToQml();
        const QString mode = setStartAction_->isChecked()
            ? QStringLiteral("start")
            : (setGoalAction_->isChecked() ? QStringLiteral("goal") : QStringLiteral("orbit"));
        statusBar()->showMessage(QStringLiteral("Interaction mode: %1").arg(mode));
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
    syncEndpointModeToQml();
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
    root->setProperty("showRoute", showRoute_);
    root->setProperty("showCollisions", showCollisions_);
    root->setProperty("showTriggers", showTriggers_);
    root->setProperty("showMovingObjects", showMovingObjects_);
    root->setProperty("showUnknowns", showUnknowns_);
}

void MainWindow::syncEndpointModeToQml() {
    if (quickView_ == nullptr || quickView_->rootObject() == nullptr) {
        return;
    }

    int mode = 0;
    if (setStartAction_ != nullptr && setStartAction_->isChecked()) {
        mode = 1;
    } else if (setGoalAction_ != nullptr && setGoalAction_->isChecked()) {
        mode = 2;
    }
    quickView_->rootObject()->setProperty("endpointMode", mode);
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

void MainWindow::chooseAndLoadRelatedSctFile() {
    if (!currentScenario_.has_value() || !currentScenario_->identity.recognized) {
        return;
    }

    const QString initialDirectory = QString::fromStdWString(
        currentScenario_->identity.mldPath.parent_path().wstring());
    const QString path = QFileDialog::getOpenFileName(this,
        "Load Related SCT",
        initialDirectory,
        "Skies SCT Files (*.sct *.SCT);;All Files (*.*)");
    if (path.isEmpty()) {
        return;
    }
    startRelatedSctLoad(path);
}

void MainWindow::startMldLoad(const QString& path) {
    if (loadWatcher_ == nullptr || loadWatcher_->isRunning() ||
        (scriptLoadWatcher_ != nullptr && scriptLoadWatcher_->isRunning())) {
        return;
    }

    diagnosticsView_->clear();
    appendDiagnosticLine(QString("Loading: %1").arg(path));
    appendDiagnosticLine("Parsing and adapting in a background worker...");
    pendingLoadPath_ = path;
    storeLastMldDirectory(path);
    updateLoadActions();
    statusBar()->showMessage(QString("Loading %1...").arg(QFileInfo(path).fileName()));

    const std::wstring nativePath = path.toStdWString();
    loadWatcher_->setFuture(QtConcurrent::run([nativePath]() {
        savor::navigation::NavigationScenarioLoader loader{};
        return loader.loadFile(std::filesystem::path(nativePath));
    }));
    updateLoadActions();
}

void MainWindow::startRelatedSctLoad(const QString& path) {
    if (!currentScenario_.has_value() || !currentScenario_->identity.recognized ||
        scriptLoadWatcher_ == nullptr || scriptLoadWatcher_->isRunning() ||
        (loadWatcher_ != nullptr && loadWatcher_->isRunning())) {
        return;
    }

    pendingSctLoadPath_ = path;
    appendDiagnosticLine(QStringLiteral("Loading related SCT: %1").arg(path));
    statusBar()->showMessage(QStringLiteral("Loading %1...").arg(QFileInfo(path).fileName()));

    const auto identity = currentScenario_->identity;
    const std::wstring nativePath = path.toStdWString();
    scriptLoadWatcher_->setFuture(QtConcurrent::run([identity, nativePath]() {
        savor::navigation::NavigationScenarioLoader loader{};
        return loader.loadRelatedScript(identity, std::filesystem::path(nativePath));
    }));
    updateLoadActions();
}

void MainWindow::finishMldLoad() {
    if (loadWatcher_ == nullptr) {
        return;
    }

    auto result = loadWatcher_->result();

    if (!result.hasModel()) {
        rebuildRecentFilesMenu();
        updateLoadActions();
        appendLoadDiagnostics(result.diagnostics);
        statusBar()->showMessage(QString("Failed to load %1").arg(QFileInfo(pendingLoadPath_).fileName()));
        QMessageBox::warning(this,
            "Load failed",
            "Could not parse the selected MLD. See Diagnostics for details.");
        pendingLoadPath_.clear();
        return;
    }

    recordRecentMldFile(pendingLoadPath_);
    currentScenario_ = std::move(*result.model);
    resetPathSelection();
    rebuildStartSelector();
    auto runtimeScene = runtimeSceneConverter_.convert(*currentScenario_);
    applyRuntimeScene(std::move(runtimeScene));
    appendScriptSummary(currentScenario_->script);
    updateLoadActions();

    const QString fileName = QFileInfo(pendingLoadPath_).fileName();
    if (!currentScenario_->isPathfindingReady()) {
        appendDiagnosticLine("WARNING: Ground or wall geometry is incomplete; pathfinding is unavailable for this model.");
        statusBar()->showMessage(QString("Loaded partial model from %1 (pathfinding unavailable)").arg(fileName));
    } else if (result.status == savor::navigation::NavigationScenarioLoadStatus::Partial) {
        statusBar()->showMessage(QString("Loaded %1 with advisory warnings (manual pathfinding available)").arg(fileName));
    } else {
        statusBar()->showMessage(QString("Loaded complete scenario from %1").arg(fileName));
    }
    const bool shouldOfferSct = currentScenario_->script.associationStatus ==
        savor::navigation::NavigationScriptAssociationStatus::Missing;
    pendingLoadPath_.clear();
    if (shouldOfferSct) {
        offerMissingRelatedSct();
    }
}

void MainWindow::finishRelatedSctLoad() {
    if (scriptLoadWatcher_ == nullptr || !currentScenario_.has_value()) {
        return;
    }

    auto result = scriptLoadWatcher_->result();
    const bool rejected = result.model.associationStatus ==
        savor::navigation::NavigationScriptAssociationStatus::Rejected;
    if (!rejected) {
        const bool replacingSelectedCatalogStart = !selectedStartOptionId_.isEmpty();
        currentScenario_->script = std::move(result.model);
        if (replacingSelectedCatalogStart) {
            selectedStartOptionId_.clear();
            startAnchor_.reset();
            startFacingYawDegrees_.reset();
            currentRoute_.reset();
        }
        rebuildStartSelector();
        if (replacingSelectedCatalogStart) {
            updateRouteOverlay();
        }
        appendScriptSummary(currentScenario_->script);
    } else {
        appendLoadDiagnostics(result.diagnostics);
        appendDiagnosticLine("Rejected SCT did not replace the current script association.");
    }

    if (!rejected && currentScenario_->script.hasDocument()) {
        statusBar()->showMessage(QStringLiteral("Loaded related SCT %1")
            .arg(QFileInfo(pendingSctLoadPath_).fileName()));
    } else if (rejected) {
        const auto selectedFileName = QFileInfo(pendingSctLoadPath_).fileName();
        const auto expectedFileName = QString::fromStdString(
            currentScenario_->identity.expectedSctFileName);
        statusBar()->showMessage(QStringLiteral("Rejected mismatched SCT %1")
            .arg(selectedFileName));
        QMessageBox::warning(this,
            "SCT does not match",
            QStringLiteral(
                "Selected: %1\nExpected: %2\n\n"
                "The existing script association was preserved.")
                .arg(selectedFileName, expectedFileName));
    } else {
        statusBar()->showMessage(QStringLiteral("Could not load related SCT %1")
            .arg(QFileInfo(pendingSctLoadPath_).fileName()));
        QMessageBox::warning(this,
            "SCT load failed",
            "The selected SCT matched the area but could not be parsed. See Diagnostics for details.");
    }
    pendingSctLoadPath_.clear();
    updateLoadActions();
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

void MainWindow::appendScriptSummary(const savor::navigation::NavigationScriptModel& script) {
    appendLoadDiagnostics(script.diagnostics);
    const QString sourcePath = script.source.path.empty()
        ? QStringLiteral("(none)")
        : QString::fromStdWString(script.source.path.wstring());
    const QString expectedPath = script.expectedPath.empty()
        ? QStringLiteral("(unavailable)")
        : QString::fromStdWString(script.expectedPath.wstring());
    appendDiagnosticLine(QStringLiteral(
        "SCT summary: source=%1, expected=%2, association=%3, parse=%4, sections=%5, exactLoopSections=%6, instructions=%7, AKLZ=%8")
        .arg(sourcePath)
        .arg(expectedPath)
        .arg(scriptAssociationLabel(script.associationStatus))
        .arg(scriptLoadLabel(script.loadStatus))
        .arg(static_cast<qulonglong>(script.sections.size()))
        .arg(static_cast<qulonglong>(script.exactLoopSectionCount))
        .arg(static_cast<qulonglong>(script.totalInstructionCount))
        .arg(script.originalCompressedAklz ? QStringLiteral("yes") : QStringLiteral("no")));
    appendDiagnosticLine(QStringLiteral(
        "SCT start catalog: opcode77=%1, options=%2, incomplete=%3")
        .arg(static_cast<qulonglong>(script.startCatalog.analyzedOpcode77Count))
        .arg(static_cast<qulonglong>(script.startCatalog.options.size()))
        .arg(static_cast<qulonglong>(script.startCatalog.incompleteOpcode77Count)));
}

void MainWindow::offerMissingRelatedSct() {
    if (!currentScenario_.has_value() ||
        currentScenario_->script.associationStatus !=
            savor::navigation::NavigationScriptAssociationStatus::Missing) {
        return;
    }

    const QString expected = QString::fromStdWString(currentScenario_->script.expectedPath.wstring());
    QMessageBox prompt(QMessageBox::Warning,
        "Related SCT not found",
        QStringLiteral("The related SCT was not found beside the MLD.\n\nExpected: %1")
            .arg(QDir::toNativeSeparators(expected)),
        QMessageBox::NoButton,
        this);
    QAbstractButton* loadButton = prompt.addButton("Load SCT...", QMessageBox::AcceptRole);
    prompt.addButton("Continue without SCT", QMessageBox::RejectRole);
    prompt.exec();
    if (prompt.clickedButton() == loadButton) {
        chooseAndLoadRelatedSctFile();
    }
}

void MainWindow::updateLoadActions() {
    const bool scenarioLoading = loadWatcher_ != nullptr && loadWatcher_->isRunning();
    const bool scriptLoading = scriptLoadWatcher_ != nullptr && scriptLoadWatcher_->isRunning();
    const bool busy = scenarioLoading || scriptLoading;
    if (busy) {
        if (setStartAction_ != nullptr) {
            const QSignalBlocker blocker(setStartAction_);
            setStartAction_->setChecked(false);
        }
        if (setGoalAction_ != nullptr) {
            const QSignalBlocker blocker(setGoalAction_);
            setGoalAction_->setChecked(false);
        }
        syncEndpointModeToQml();
    }
    if (openAction_ != nullptr) {
        openAction_->setEnabled(!busy);
    }
    if (recentFilesMenu_ != nullptr) {
        recentFilesMenu_->setEnabled(!busy && !recentMldFiles_.isEmpty());
    }
    if (loadRelatedSctAction_ != nullptr) {
        loadRelatedSctAction_->setEnabled(!busy && currentScenario_.has_value() &&
            currentScenario_->identity.recognized);
    }
    const bool canPickPath = !busy && currentScenario_.has_value() &&
        currentScenario_->isPathfindingReady();
    if (setStartAction_ != nullptr) {
        setStartAction_->setEnabled(canPickPath);
    }
    if (setGoalAction_ != nullptr) {
        setGoalAction_->setEnabled(canPickPath);
    }
    if (startSelector_ != nullptr) {
        startSelector_->setEnabled(canPickPath);
    }
    if (manualStartFacingSpinBox_ != nullptr) {
        manualStartFacingSpinBox_->setEnabled(canPickPath);
    }
}

void MainWindow::resetPathSelection() {
    selectedStartOptionId_.clear();
    startAnchor_.reset();
    startFacingYawDegrees_.reset();
    goalAnchor_.reset();
    triggerGoalTarget_.reset();
    currentRoute_.reset();
    if (setStartAction_ != nullptr) {
        const QSignalBlocker blocker(setStartAction_);
        setStartAction_->setChecked(false);
    }
    if (setGoalAction_ != nullptr) {
        const QSignalBlocker blocker(setGoalAction_);
        setGoalAction_->setChecked(false);
    }
    syncEndpointModeToQml();
    if (startSelector_ != nullptr && startSelector_->count() > 0) {
        const QSignalBlocker blocker(startSelector_);
        startSelector_->setCurrentIndex(0);
    }
    applyRouteData(RuntimeRouteData{});
}

void MainWindow::rebuildStartSelector() {
    if (startSelector_ == nullptr) {
        return;
    }

    const QSignalBlocker blocker(startSelector_);
    auto* model = qobject_cast<QStandardItemModel*>(startSelector_->model());
    if (model == nullptr) {
        model = new QStandardItemModel(startSelector_);
        startSelector_->setModel(model);
    }
    model->clear();

    auto* manualItem = new QStandardItem(QStringLiteral("Manual point"));
    manualItem->setData(QString{}, kStartOptionIdRole);
    manualItem->setToolTip(QStringLiteral("Choose Set Start, then click a walkable ground surface."));
    model->appendRow(manualItem);

    if (!currentScenario_.has_value()) {
        startSelector_->setCurrentIndex(0);
        return;
    }

    const auto appendGroup = [model](const QString& title,
                                 const std::vector<const savor::navigation::NavigationStartOption*>& options) {
        if (options.empty()) {
            return;
        }
        auto* header = new QStandardItem(title);
        header->setFlags(Qt::NoItemFlags);
        model->appendRow(header);
        for (const auto* option : options) {
            auto* item = new QStandardItem(QString::fromStdString(option->label));
            item->setData(QString::fromStdString(option->id), kStartOptionIdRole);
            item->setToolTip(startOptionToolTip(*option));
            if (option->availability != savor::navigation::NavigationStartAvailability::Resolvable) {
                item->setFlags(item->flags() & ~Qt::ItemIsEnabled & ~Qt::ItemIsSelectable);
            }
            model->appendRow(item);
        }
    };

    std::vector<const savor::navigation::NavigationStartOption*> authored{};
    std::vector<const savor::navigation::NavigationStartOption*> scripted{};
    std::vector<const savor::navigation::NavigationStartOption*> incomplete{};
    for (const auto& option : currentScenario_->script.startCatalog.options) {
        if (option.availability != savor::navigation::NavigationStartAvailability::Resolvable) {
            incomplete.push_back(&option);
        } else if (option.kind == savor::navigation::NavigationStartKind::AuthoredArrival) {
            authored.push_back(&option);
        } else {
            scripted.push_back(&option);
        }
    }
    appendGroup(QStringLiteral("Authored arrivals"), authored);
    appendGroup(QStringLiteral("Scripted repositions"), scripted);
    appendGroup(QStringLiteral("Incomplete placements"), incomplete);

    int selectedIndex = 0;
    if (!selectedStartOptionId_.isEmpty()) {
        const int found = startSelector_->findData(selectedStartOptionId_, kStartOptionIdRole);
        if (found >= 0) {
            selectedIndex = found;
        } else {
            selectedStartOptionId_.clear();
        }
    }
    startSelector_->setCurrentIndex(selectedIndex);
}

void MainWindow::handleStartSelectorChanged(const int index) {
    if (startSelector_ == nullptr || index < 0) {
        return;
    }

    const QString optionId = startSelector_->itemData(index, kStartOptionIdRole).toString();
    if (optionId.isEmpty()) {
        // Selecting Manual point changes how the next start is chosen; it does
        // not invalidate the current anchor. Its SCT provenance remains tracked
        // until a manual pick succeeds, so replacing that SCT cannot leave a
        // stale script-derived start in place.
        return;
    }
    if (!currentScenario_.has_value() || !currentScenario_->isPathfindingReady()) {
        const QSignalBlocker blocker(startSelector_);
        const int previous = selectedStartOptionId_.isEmpty()
            ? 0
            : startSelector_->findData(selectedStartOptionId_, kStartOptionIdRole);
        startSelector_->setCurrentIndex(previous >= 0 ? previous : 0);
        return;
    }

    const auto& options = currentScenario_->script.startCatalog.options;
    const auto found = std::find_if(options.begin(), options.end(), [&optionId](const auto& option) {
        return QString::fromStdString(option.id) == optionId;
    });
    if (found == options.end()) {
        appendDiagnosticLine(QStringLiteral("WARNING: Selected SCT start is no longer available."));
        rebuildStartSelector();
        return;
    }

    savor::navigation::NavigationStartResolver resolver{};
    const auto result = resolver.resolve(
        currentScenario_->area,
        currentScenario_->traversalGraph,
        *found);
    if (!result.hasAnchor()) {
        appendDiagnosticLine(QStringLiteral("WARNING: Could not resolve SCT start '%1': %2")
            .arg(QString::fromStdString(found->label), QString::fromStdString(result.message)));
        statusBar()->showMessage(QStringLiteral("Could not use SCT start: %1")
            .arg(QString::fromStdString(result.message)));
        const QSignalBlocker blocker(startSelector_);
        const int previous = selectedStartOptionId_.isEmpty()
            ? 0
            : startSelector_->findData(selectedStartOptionId_, kStartOptionIdRole);
        startSelector_->setCurrentIndex(previous >= 0 ? previous : 0);
        return;
    }

    selectedStartOptionId_ = optionId;
    startAnchor_ = result.anchor;
    startFacingYawDegrees_ = result.yawDegrees;
    if (setStartAction_ != nullptr) {
        const QSignalBlocker blocker(setStartAction_);
        setStartAction_->setChecked(false);
    }
    syncEndpointModeToQml();
    const auto& anchor = *result.anchor;
    appendDiagnosticLine(QStringLiteral(
        "SCT start anchored: option=%1, ground=%2, surface=%3, triangle=%4, snapDistance=%5, yaw=%6")
        .arg(QString::fromStdString(found->label))
        .arg(found->groundTblId.value_or(-1))
        .arg(static_cast<qulonglong>(anchor.triangle.surfaceIndex))
        .arg(static_cast<qulonglong>(anchor.triangle.triangleIndex))
        .arg(anchor.snapDistance, 0, 'g', 7)
        .arg(result.yawDegrees.has_value()
            ? QString::number(*result.yawDegrees, 'g', 7)
            : QStringLiteral("unknown")));
    updateRouteOverlay();
}

void MainWindow::handleGroundPick(const int surfaceIndex,
    const float x,
    const float y,
    const float z) {
    const bool busy = (loadWatcher_ != nullptr && loadWatcher_->isRunning()) ||
        (scriptLoadWatcher_ != nullptr && scriptLoadWatcher_->isRunning());
    if (busy || !currentScenario_.has_value() || !currentScenario_->isPathfindingReady() || surfaceIndex < 0) {
        appendDiagnosticLine("Ground pick ignored because the current scenario is not pathfinding-ready.");
        return;
    }

    const bool settingStart = setStartAction_ != nullptr && setStartAction_->isChecked();
    const bool settingGoal = setGoalAction_ != nullptr && setGoalAction_->isChecked();
    if (!settingStart && !settingGoal) {
        return;
    }

    savor::navigation::NavigationGraphAnchorer anchorer{};
    const auto anchorResult = anchorer.anchorToSurface(
        currentScenario_->traversalGraph,
        static_cast<std::size_t>(surfaceIndex),
        savor::navigation::NavigationVec3{ x, y, z });
    if (!anchorResult.hasAnchor()) {
        appendDiagnosticLine(QStringLiteral("WARNING: Could not anchor ground pick: %1")
            .arg(QString::fromStdString(anchorResult.message)));
        statusBar()->showMessage("Could not anchor the selected ground point.");
        return;
    }

    const auto& anchor = *anchorResult.anchor;
    if (settingStart) {
        startAnchor_ = anchor;
        startFacingYawDegrees_ = manualStartYawDegrees_;
        selectedStartOptionId_.clear();
        if (startSelector_ != nullptr && startSelector_->count() > 0) {
            const QSignalBlocker blocker(startSelector_);
            startSelector_->setCurrentIndex(0);
        }
    } else {
        goalAnchor_ = anchor;
        triggerGoalTarget_.reset();
    }
    appendDiagnosticLine(QStringLiteral("%1 anchored: surface=%2, triangle=%3, snapDistance=%4")
        .arg(settingStart ? QStringLiteral("Start") : QStringLiteral("Goal"))
        .arg(static_cast<qulonglong>(anchor.triangle.surfaceIndex))
        .arg(static_cast<qulonglong>(anchor.triangle.triangleIndex))
        .arg(anchor.snapDistance, 0, 'g', 7));
    updateRouteOverlay();
}

void MainWindow::handleTriggerGoalPick(const int regionIndex) {
    const bool busy = (loadWatcher_ != nullptr && loadWatcher_->isRunning()) ||
        (scriptLoadWatcher_ != nullptr && scriptLoadWatcher_->isRunning());
    const bool settingGoal = setGoalAction_ != nullptr && setGoalAction_->isChecked();
    if (busy || !settingGoal || !currentScenario_.has_value() ||
        !currentScenario_->isPathfindingReady() || regionIndex < 0) {
        appendDiagnosticLine("Trigger pick ignored because a pathfinding-ready goal selection is not active.");
        return;
    }

    savor::navigation::NavigationTriggerGoalResolver resolver{};
    const auto result = resolver.resolve(
        currentScenario_->area,
        currentScenario_->traversalGraph,
        static_cast<std::size_t>(regionIndex));
    if (!result.hasTarget()) {
        appendDiagnosticLine(QStringLiteral("WARNING: Could not resolve trigger goal: %1")
            .arg(QString::fromStdString(result.message)));
        statusBar()->showMessage(QStringLiteral("Could not use trigger goal: %1")
            .arg(QString::fromStdString(result.message)));
        return;
    }

    triggerGoalTarget_ = result.target;
    goalAnchor_ = result.target->anchor;
    const auto& target = *result.target;
    appendDiagnosticLine(QStringLiteral(
        "Trigger goal anchored: region=%1, primaryMesh=%2, surface=%3, triangle=%4, snapDistance=%5, boundsDiagonalSquared=%6")
        .arg(static_cast<qulonglong>(target.regionIndex))
        .arg(static_cast<qulonglong>(target.primaryMeshIndex))
        .arg(static_cast<qulonglong>(target.anchor.triangle.surfaceIndex))
        .arg(static_cast<qulonglong>(target.anchor.triangle.triangleIndex))
        .arg(target.anchor.snapDistance, 0, 'g', 7)
        .arg(target.primaryMeshAabbSquaredDiagonal, 0, 'g', 7));
    updateRouteOverlay();
}

void MainWindow::updateRouteOverlay() {
    currentRoute_.reset();
    if (currentScenario_.has_value() && startAnchor_.has_value() && goalAnchor_.has_value()) {
        savor::navigation::NavigationPathfinder pathfinder{};
        currentRoute_ = pathfinder.findPath(
            currentScenario_->traversalGraph,
            savor::navigation::NavigationPathQuery{
                .start = *startAnchor_,
                .goal = *goalAnchor_,
            });
        appendDiagnosticLine(QStringLiteral("Path result: %1")
            .arg(QString::fromStdString(currentRoute_->message)));
        if (currentRoute_->hasPath()) {
            appendDiagnosticLine(QStringLiteral("Path metrics: triangles=%1, points=%2, length=%3, cost=%4")
                .arg(static_cast<qulonglong>(currentRoute_->trianglePath.size()))
                .arg(static_cast<qulonglong>(currentRoute_->polyline.size()))
                .arg(currentRoute_->routeLength, 0, 'g', 8)
                .arg(currentRoute_->totalCost, 0, 'g', 8));
            statusBar()->showMessage(QStringLiteral("Path found: length %1, cost %2")
                .arg(currentRoute_->routeLength, 0, 'g', 7)
                .arg(currentRoute_->totalCost, 0, 'g', 7));
        } else {
            statusBar()->showMessage(QStringLiteral("Path unavailable: %1")
                .arg(QString::fromStdString(currentRoute_->message)));
        }
    } else if (startAnchor_.has_value() || goalAnchor_.has_value()) {
        statusBar()->showMessage(startAnchor_.has_value()
            ? QStringLiteral("Start set; choose Set Goal and pick a ground or trigger.")
            : QStringLiteral("Goal set; choose Set Start and pick the ground."));
    }

    renderRouteOverlay();
}

void MainWindow::renderRouteOverlay(const bool includeDiagnostics) {
    RuntimeRouteData data = runtimeSceneConverter_.convertRoute(
        startAnchor_, startFacingYawDegrees_, goalAnchor_, triggerGoalTarget_, currentRoute_, sceneExtent_);
    if (!includeDiagnostics) {
        data.diagnostics.clear();
    }
    applyRouteData(std::move(data));
}

void MainWindow::applyRouteData(RuntimeRouteData data) {
    const bool preserveVisible = showRoute_;
    auto previousGeometry = std::move(routeGeometryStore_);
    routeGeometryStore_ = std::move(data.geometries);
    routeMeshes_ = std::move(data.routes);
    for (int index = 0; index < routeMeshes_.size(); ++index) {
        QVariantMap item = routeMeshes_.at(index).toMap();
        item.insert("visible", preserveVisible);
        routeMeshes_[index] = item;
    }
    for (const auto& diagnostic : data.diagnostics) {
        appendDiagnosticLine(QString::fromStdString(diagnostic));
    }
    if (visibilityWidget_ != nullptr) {
        visibilityWidget_->setLayers(groundMeshes_,
            linkMeshes_,
            routeMeshes_,
            collisionMeshes_,
            triggerMeshes_,
            movingObjectMeshes_,
            unknownMeshes_);
    }
    applyMeshesToQml();
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
    applyDefaultVisibility(routeMeshes_);
    applyDefaultVisibility(collisionMeshes_);
    applyDefaultVisibility(triggerMeshes_);
    applyDefaultVisibility(movingObjectMeshes_);
    applyDefaultVisibility(unknownMeshes_);

    setAllVisibility(true);

    QObject* root = quickView_->rootObject();
    root->setProperty("cameraTarget", QVariant::fromValue(data.center));
    root->setProperty("cameraDistance", data.extent * 1.75f);
    sceneExtent_ = data.extent;

    for (const auto& line : data.diagnostics) {
        appendDiagnosticLine(QString::fromStdString(line));
    }

    statusBar()->showMessage(QString("Scene updated: Grounds=%1, Links=%2, Route=%3, Collisions=%4, Triggers=%5, MovingObjects=%6, Unknown=%7")
        .arg(static_cast<int>(groundMeshes_.size()))
        .arg(static_cast<int>(linkMeshes_.size()))
        .arg(static_cast<int>(routeMeshes_.size()))
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
    root->setProperty("routeMeshes", routeMeshes_);
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
    if (!routeMeshes_.empty()) {
        showRoute_ = hasVisible(routeMeshes_);
    }
    showCollisions_ = hasVisible(collisionMeshes_);
    showTriggers_ = hasVisible(triggerMeshes_);
    showMovingObjects_ = hasVisible(movingObjectMeshes_);
    showUnknowns_ = hasVisible(unknownMeshes_);

    setActionCheckedNoSignal(groundsAction_, showGrounds_);
    setActionCheckedNoSignal(linksAction_, showLinks_);
    setActionCheckedNoSignal(routeAction_, showRoute_);
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
        groundMeshes_, linkMeshes_, routeMeshes_, collisionMeshes_, triggerMeshes_, movingObjectMeshes_, unknownMeshes_);
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
    setMeshes(routeMeshes_);
    setMeshes(collisionMeshes_);
    setMeshes(triggerMeshes_);
    setMeshes(movingObjectMeshes_);
    setMeshes(unknownMeshes_);

    visibilityWidget_->setLayers(
        groundMeshes_, linkMeshes_, routeMeshes_, collisionMeshes_, triggerMeshes_, movingObjectMeshes_, unknownMeshes_);
    applyMeshesToQml();
}

QVariantList* MainWindow::meshesForLayer(const VisibilityTreeWidget::LayerKind layer) {
    switch (layer) {
    case VisibilityTreeWidget::LayerKind::Grounds:
        return &groundMeshes_;
    case VisibilityTreeWidget::LayerKind::Links:
        return &linkMeshes_;
    case VisibilityTreeWidget::LayerKind::Route:
        return &routeMeshes_;
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
    appendDiagnosticLine(QString("[VisibilityDebug] %1").arg(summarizeLayer(QStringLiteral("Route"), routeMeshes_)));
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
