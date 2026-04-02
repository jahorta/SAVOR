#include "SettingsPage.h"

#include "DB/DBCore/DbSnapshotService.h"
#include "DB/DBCore/DbService.h"
#include "DB/Querying/DataService.h"
#include "GUI/Panes/CoordinatorPane/CoordinatorController.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <exception>
#include <filesystem>
#include <string>
#include <utility>

namespace {
constexpr auto kSettingsGroup = "Settings";
constexpr auto kDbRootKey = "db_root";

using StorageResult = std::pair<bool, QString>;

template <typename AsyncCall>
auto runAsync(AsyncCall&& asyncCall)
{
    return QtConcurrent::run([call = std::forward<AsyncCall>(asyncCall)]() mutable {
        return call();
    });
}

QString statusKindToString(SettingsPage::StatusKind kind)
{
    switch (kind) {
    case SettingsPage::StatusKind::Info:
        return "info";
    case SettingsPage::StatusKind::Warning:
        return "warning";
    case SettingsPage::StatusKind::Success:
        return "success";
    case SettingsPage::StatusKind::Failure:
        return "failure";
    case SettingsPage::StatusKind::Working:
        return "working";
    }

    return "info";
}
} // namespace

SettingsPage::SettingsPage(CoordinatorController* coordinatorController, QWidget* parent)
    : QWidget(parent)
    , coordinatorController_(coordinatorController)
{
    connect(&storageWatcher_, &QFutureWatcher<StorageResult>::finished, this, &SettingsPage::handleStorageOperationFinished);
    connect(&reconcileWatcher_, &QFutureWatcher<simcore::db::DbResult<simcore::db::ExplorerRunReconcileResult>>::finished, this, &SettingsPage::handleReconcileOperationFinished);

    createWidgets();
    loadState();
    refreshCoordinatorUi();
    refreshStorageUi();

    if (coordinatorController_) {
        connect(coordinatorController_, &CoordinatorController::stateChanged, this, &SettingsPage::refreshCoordinatorUi);
    }
}

void SettingsPage::focusCoordinatorSettings(CoordinatorFocusTarget target)
{
    if (coordinatorSectionToggle_ && !coordinatorSectionToggle_->isChecked()) {
        coordinatorSectionToggle_->setChecked(true);
    }

    QWidget* focusTarget = nullptr;
    switch (target) {
    case CoordinatorFocusTarget::IsoPath:
        focusTarget = isoPathEdit_;
        break;
    case CoordinatorFocusTarget::DolphinBaseDir:
        focusTarget = dolphinBaseDirEdit_;
        break;
    case CoordinatorFocusTarget::Section:
        focusTarget = coordinatorSectionToggle_;
        break;
    }

    if (focusTarget) {
        focusTarget->setFocus(Qt::OtherFocusReason);
    }
}

void SettingsPage::createWidgets()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(10);

    const CollapsibleSection storageSection = createCollapsibleSection(
        "SETTINGS SECTION",
        "Database Storage",
        "Manage the active SoaSim database root, switch to an existing database, and save or load portable database snapshots with artifacts.");

    QVBoxLayout* storageLayout = new QVBoxLayout(storageSection.content);
    storageLayout->setContentsMargins(0, 0, 0, 0);
    storageLayout->setSpacing(10);

    QGridLayout* formLayout = new QGridLayout();
    formLayout->setHorizontalSpacing(10);
    formLayout->setVerticalSpacing(10);

    QLabel* activeRootLabel = new QLabel("Current database root", storageSection.content);
    activeRootLabel->setObjectName("settingsFieldLabel");
    activeRootValueLabel_ = new QLabel(storageSection.content);
    activeRootValueLabel_->setObjectName("settingsValueLabel");
    activeRootValueLabel_->setWordWrap(true);

    moveDatabaseButton_ = new QPushButton("Move Database…", storageSection.content);
    moveDatabaseButton_->setObjectName("jobsPrimaryButton");
    connect(moveDatabaseButton_, &QPushButton::clicked, this, &SettingsPage::handleMoveDatabaseClicked);

    formLayout->addWidget(activeRootLabel, 0, 0);
    formLayout->addWidget(activeRootValueLabel_, 0, 1);
    formLayout->addWidget(moveDatabaseButton_, 0, 2);
    formLayout->setColumnStretch(1, 1);
    storageLayout->addLayout(formLayout);

    QHBoxLayout* switchLayout = new QHBoxLayout();

    QLabel* switchDescription = new QLabel(
        "Point SoaSimQt at another existing database root without copying data. The selected directory must contain SoaSimDB.sqlite3.",
        storageSection.content);
    switchDescription->setObjectName("settingsSectionDescription");
    switchDescription->setWordWrap(true);
    switchLayout->addWidget(switchDescription, 1);

    switchLayout->setSpacing(10);
    switchLayout->addStretch();
    useExistingButton_ = new QPushButton("Use Existing Database…", storageSection.content);
    useExistingButton_->setObjectName("jobsSecondaryButton");
    connect(useExistingButton_, &QPushButton::clicked, this, &SettingsPage::handleUseExistingDatabaseClicked);
    switchLayout->addWidget(useExistingButton_);
    storageLayout->addLayout(switchLayout);

    QHBoxLayout* resetLayout = new QHBoxLayout();

    QLabel* resetDescription = new QLabel(
        "Delete the active database root and recreate it from scratch. This removes the SQLite database, stored artifacts, and temporary files in the active root.",
        storageSection.content);
    resetDescription->setObjectName("settingsSectionDescription");
    resetDescription->setWordWrap(true);
    resetLayout->addWidget(resetDescription, 1);

    resetLayout->setSpacing(10);
    resetLayout->addStretch();
    resetDatabaseButton_ = new QPushButton("Delete && Remake Database…", storageSection.content);
    resetDatabaseButton_->setObjectName("jobsSecondaryButton");
    connect(resetDatabaseButton_, &QPushButton::clicked, this, &SettingsPage::handleResetDatabaseClicked);
    resetLayout->addWidget(resetDatabaseButton_);
    storageLayout->addLayout(resetLayout);

    QHBoxLayout* snapshotLayout = new QHBoxLayout();

    QLabel* snapshotDescription = new QLabel(
        "Save a compressed snapshot of the database plus object-store artifacts, or load a snapshot into a target root. Snapshots do not include temporary cache files.",
        storageSection.content);
    snapshotDescription->setObjectName("settingsSectionDescription");
    snapshotDescription->setWordWrap(true);
    snapshotLayout->addWidget(snapshotDescription, 1);

    snapshotLayout->setSpacing(10);
    snapshotLayout->addStretch();
    saveSnapshotButton_ = new QPushButton("Save Snapshot…", storageSection.content);
    saveSnapshotButton_->setObjectName("jobsSecondaryButton");
    connect(saveSnapshotButton_, &QPushButton::clicked, this, &SettingsPage::handleSaveSnapshotClicked);
    loadSnapshotButton_ = new QPushButton("Load Snapshot…", storageSection.content);
    loadSnapshotButton_->setObjectName("jobsSecondaryButton");
    connect(loadSnapshotButton_, &QPushButton::clicked, this, &SettingsPage::handleLoadSnapshotClicked);
    snapshotLayout->addWidget(saveSnapshotButton_);
    snapshotLayout->addWidget(loadSnapshotButton_);
    storageLayout->addLayout(snapshotLayout);

    statusLabel_ = new QLabel(storageSection.content);
    statusLabel_->setObjectName("settingsStatus");
    statusLabel_->setWordWrap(true);
    storageLayout->addWidget(statusLabel_);

    rootLayout->addWidget(storageSection.card);

    const CollapsibleSection coordinatorSection = createCollapsibleSection(
        "RUNTIME SECTION",
        "Coordinator",
        "Coordinator startup defaults now live here so the Workers page can focus on runtime control and telemetry.");
    coordinatorSectionToggle_ = coordinatorSection.toggleButton;

    QVBoxLayout* coordinatorLayout = new QVBoxLayout(coordinatorSection.content);
    coordinatorLayout->setContentsMargins(0, 0, 0, 0);
    coordinatorLayout->setSpacing(10);

    QGridLayout* coordinatorFormLayout = new QGridLayout();
    coordinatorFormLayout->setHorizontalSpacing(10);
    coordinatorFormLayout->setVerticalSpacing(10);

    isoPathEdit_ = new QLineEdit(coordinatorSection.content);
    isoPathEdit_->setPlaceholderText("Path to SkiesOfArcadia iso");
    isoBrowseButton_ = new QPushButton("Browse…", coordinatorSection.content);

    dolphinBaseDirEdit_ = new QLineEdit(coordinatorSection.content);
    dolphinBaseDirEdit_->setPlaceholderText("Path to DolphinQt base directory with portable.txt");
    dolphinBrowseButton_ = new QPushButton("Browse…", coordinatorSection.content);

    eventBufferSpin_ = new QSpinBox(coordinatorSection.content);
    eventBufferSpin_->setObjectName("jobsRefreshSpin");
    eventBufferSpin_->setMinimum(8);
    eventBufferSpin_->setMaximum(1000000);

    startPausedCheck_ = new QCheckBox("Start paused", coordinatorSection.content);
    requeueFailuresAutomaticallyCheck_ = new QCheckBox("Requeue failures automatically", coordinatorSection.content);

    QHBoxLayout* isoLayout = new QHBoxLayout();
    isoLayout->setContentsMargins(0, 0, 0, 0);
    isoLayout->setSpacing(8);
    isoLayout->addWidget(isoPathEdit_, 1);
    isoLayout->addWidget(isoBrowseButton_);

    QHBoxLayout* dolphinLayout = new QHBoxLayout();
    dolphinLayout->setContentsMargins(0, 0, 0, 0);
    dolphinLayout->setSpacing(8);
    dolphinLayout->addWidget(dolphinBaseDirEdit_, 1);
    dolphinLayout->addWidget(dolphinBrowseButton_);

    QHBoxLayout* startupLayout = new QHBoxLayout();
    startupLayout->setContentsMargins(0, 0, 0, 0);
    startupLayout->setSpacing(10);
    startupLayout->addWidget(eventBufferSpin_);
    startupLayout->addWidget(startPausedCheck_);
    startupLayout->addWidget(requeueFailuresAutomaticallyCheck_);
    startupLayout->addStretch();

    QLabel* isoLabel = new QLabel("ISO", coordinatorSection.content);
    isoLabel->setObjectName("settingsFieldLabel");
    QLabel* dolphinLabel = new QLabel("Dolphin base", coordinatorSection.content);
    dolphinLabel->setObjectName("settingsFieldLabel");
    QLabel* startupLabel = new QLabel("Buffer + startup", coordinatorSection.content);
    startupLabel->setObjectName("settingsFieldLabel");

    coordinatorFormLayout->addWidget(isoLabel, 0, 0);
    coordinatorFormLayout->addLayout(isoLayout, 0, 1);
    coordinatorFormLayout->addWidget(dolphinLabel, 1, 0);
    coordinatorFormLayout->addLayout(dolphinLayout, 1, 1);
    coordinatorFormLayout->addWidget(startupLabel, 2, 0);
    coordinatorFormLayout->addLayout(startupLayout, 2, 1);
    coordinatorFormLayout->setColumnStretch(1, 1);
    coordinatorLayout->addLayout(coordinatorFormLayout);

    coordinatorValidationLabel_ = new QLabel(coordinatorSection.content);
    coordinatorValidationLabel_->setObjectName("coordinatorValidation");
    coordinatorValidationLabel_->setWordWrap(true);
    coordinatorLayout->addWidget(coordinatorValidationLabel_);

    if (coordinatorController_) {
        connect(isoPathEdit_, &QLineEdit::textChanged, coordinatorController_, &CoordinatorController::setIsoPath);
        connect(dolphinBaseDirEdit_, &QLineEdit::textChanged, coordinatorController_, &CoordinatorController::setDolphinBaseDir);
        connect(eventBufferSpin_, qOverload<int>(&QSpinBox::valueChanged), coordinatorController_, &CoordinatorController::setEventBufferCapacity);
        connect(startPausedCheck_, &QCheckBox::toggled, coordinatorController_, &CoordinatorController::setStartPaused);
        connect(requeueFailuresAutomaticallyCheck_, &QCheckBox::toggled, coordinatorController_, &CoordinatorController::setRestartFailedJobsAutomatically);
    }
    connect(isoBrowseButton_, &QPushButton::clicked, this, &SettingsPage::browseForIsoPath);
    connect(dolphinBrowseButton_, &QPushButton::clicked, this, &SettingsPage::browseForDolphinBaseDir);

    rootLayout->addWidget(coordinatorSection.card);

    const CollapsibleSection reconcileSection = createCollapsibleSection(
        "MAINTENANCE SECTION",
        "Reconcile DB changes",
        "Run one-shot maintenance routines that backfill data introduced by newer schema and detection logic.");

    QVBoxLayout* reconcileLayout = new QVBoxLayout(reconcileSection.content);
    reconcileLayout->setContentsMargins(0, 0, 0, 0);
    reconcileLayout->setSpacing(10);

    QLabel* reconcileDescription = new QLabel(
        "Populate explorer_run rows for historical Explorer / BattleSingleTurn root job-set trees that predate explorer_run linkage.",
        reconcileSection.content);
    reconcileDescription->setObjectName("settingsSectionDescription");
    reconcileDescription->setWordWrap(true);
    reconcileLayout->addWidget(reconcileDescription);

    QHBoxLayout* reconcileButtonLayout = new QHBoxLayout();
    reconcileButtonLayout->setContentsMargins(0, 0, 0, 0);
    reconcileButtonLayout->addStretch();
    reconcileExplorerRunsButton_ = new QPushButton("Backfill Explorer Runs", reconcileSection.content);
    reconcileExplorerRunsButton_->setObjectName("jobsSecondaryButton");
    connect(reconcileExplorerRunsButton_, &QPushButton::clicked, this, &SettingsPage::handleReconcileExplorerRunsClicked);
    reconcileButtonLayout->addWidget(reconcileExplorerRunsButton_);
    reconcileLayout->addLayout(reconcileButtonLayout);

    rootLayout->addWidget(reconcileSection.card);

    const CollapsibleSection futureSection = createCollapsibleSection(
        "PLACEHOLDER",
        "Future sections",
        "Scaffold reserved for additional SoaSimQt settings sections such as worker defaults, diagnostics, and UI preferences.",
        false);

    QVBoxLayout* futureLayout = new QVBoxLayout(futureSection.content);
    futureLayout->setContentsMargins(0, 0, 0, 0);
    futureLayout->setSpacing(8);

    QLabel* futureBody = new QLabel(
        "Additional sections can be dropped into this collapsible pattern without changing the overall Settings page layout.",
        futureSection.content);
    futureBody->setObjectName("settingsSectionDescription");
    futureBody->setWordWrap(true);
    futureLayout->addWidget(futureBody);

    rootLayout->addWidget(futureSection.card);
    rootLayout->addStretch();
}

SettingsPage::CollapsibleSection SettingsPage::createCollapsibleSection(
    const QString& eyebrow,
    const QString& title,
    const QString& description,
    bool expandedByDefault)
{
    CollapsibleSection section;
    section.card = new QFrame(this);
    section.card->setObjectName("settingsCard");

    QVBoxLayout* cardLayout = new QVBoxLayout(section.card);
    cardLayout->setContentsMargins(14, 14, 14, 14);
    cardLayout->setSpacing(10);

    QLabel* sectionEyebrow = new QLabel(eyebrow, section.card);
    sectionEyebrow->setObjectName("settingsSectionEyebrow");
    cardLayout->addWidget(sectionEyebrow);

    section.toggleButton = new QToolButton(section.card);
    section.toggleButton->setObjectName("settingsSectionToggle");
    section.toggleButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    section.toggleButton->setArrowType(expandedByDefault ? Qt::DownArrow : Qt::RightArrow);
    section.toggleButton->setText(title);
    section.toggleButton->setCheckable(true);
    section.toggleButton->setChecked(expandedByDefault);
    cardLayout->addWidget(section.toggleButton);

    QLabel* sectionDescription = new QLabel(description, section.card);
    sectionDescription->setObjectName("settingsSectionDescription");
    sectionDescription->setWordWrap(true);
    cardLayout->addWidget(sectionDescription);

    section.content = new QWidget(section.card);
    section.content->setVisible(expandedByDefault);
    cardLayout->addWidget(section.content);

    connect(section.toggleButton, &QToolButton::toggled, section.content, &QWidget::setVisible);
    connect(section.toggleButton, &QToolButton::toggled, section.toggleButton, [toggle = section.toggleButton](bool checked) {
        toggle->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
    });

    return section;
}

void SettingsPage::loadState()
{
    refreshActiveRoot();

    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    persistedRoot_ = normalizePath(settings.value(kDbRootKey).toString().trimmed());
    settings.endGroup();

    if (persistedRoot_.isEmpty()) {
        setStatus(StatusKind::Info, "Using the active database root because no Qt-specific saved storage path exists yet.");
    } else {
        setStatus(StatusKind::Info, "Loaded the saved database root preference from SoaSimQt settings.");
    }
}

void SettingsPage::refreshActiveRoot()
{
    activeRoot_ = normalizePath(QString::fromStdString(simcore::db::DBService::instance().database_root().string()));
    activeRootValueLabel_->setText(activeRoot_.isEmpty() ? "(unknown)" : activeRoot_);
}

void SettingsPage::refreshStorageUi()
{
    refreshActiveRoot();
    const bool enabled = !storageBusy_ && !reconcileBusy_;
    if (moveDatabaseButton_) moveDatabaseButton_->setEnabled(enabled);
    if (resetDatabaseButton_) resetDatabaseButton_->setEnabled(enabled);
    if (useExistingButton_) useExistingButton_->setEnabled(enabled);
    if (saveSnapshotButton_) saveSnapshotButton_->setEnabled(enabled);
    if (loadSnapshotButton_) loadSnapshotButton_->setEnabled(enabled);
    if (reconcileExplorerRunsButton_) reconcileExplorerRunsButton_->setEnabled(enabled);
}

void SettingsPage::handleMoveDatabaseClicked()
{
    const QString startDir = activeRoot_.isEmpty() ? QDir::homePath() : activeRoot_;
    const QString selectedDir = QFileDialog::getExistingDirectory(this, "Select new database root", startDir);
    if (selectedDir.isEmpty()) {
        return;
    }

    const QString targetRoot = normalizePath(selectedDir);
    if (targetRoot.isEmpty()) {
        setStatus(StatusKind::Warning, "Select a valid database root directory.");
        return;
    }
    if (targetRoot == activeRoot_) {
        setStatus(StatusKind::Info, "Selected directory is already the active database root.");
        return;
    }

    QFileInfo targetInfo(targetRoot);
    const QFileInfo parentInfo(targetInfo.dir().absolutePath());
    if ((targetInfo.exists() && !targetInfo.isDir()) || !parentInfo.exists() || !parentInfo.isDir()) {
        setStatus(StatusKind::Warning, "The selected destination directory is not reachable.");
        return;
    }

    const auto answer = QMessageBox::question(
        this,
        "Move Database",
        QStringLiteral("Move the active database to a new root?\n\nCurrent root:\n%1\n\nNew root:\n%2\n\nSoaSimQt will stop database services, copy the database and artifacts, switch to the new root, and then delete the previous root contents.")
            .arg(activeRoot_, targetRoot),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }

    startStorageOperation(
        StorageOperation::MoveDatabase,
        QStringLiteral("Moving database storage to %1…").arg(targetRoot),
        [target = targetRoot.toStdString()]() {
            std::string error;
            const bool ok = simcore::db::DBService::instance().relocate_database_root(target, true, error);
            return StorageResult{ ok, ok ? QString() : QString::fromStdString(error) };
        });
}

void SettingsPage::handleResetDatabaseClicked()
{
    if (activeRoot_.isEmpty()) {
        setStatus(StatusKind::Warning, "The active database root is unknown, so it cannot be reset.");
        return;
    }

    const auto answer = QMessageBox::warning(
        this,
        "Delete and Remake Database",
        QStringLiteral("Delete and remake the active database root?\n\nActive root:\n%1\n\nThis permanently deletes the SQLite database, stored artifacts, and temporary files in this root. SoaSimQt will then recreate a fresh empty database in the same location.")
            .arg(activeRoot_),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }

    bool confirmed = false;
    const QString confirmationText = QInputDialog::getText(
        this,
        "Type delete to confirm",
        QStringLiteral("Type delete to permanently remove and recreate this database root:\n%1").arg(activeRoot_),
        QLineEdit::Normal,
        QString(),
        &confirmed);
    if (!confirmed) {
        return;
    }
    if (confirmationText.trimmed() != QStringLiteral("delete")) {
        setStatus(StatusKind::Warning, "Database reset cancelled because the confirmation text did not match \"delete\".");
        return;
    }

    startStorageOperation(
        StorageOperation::ResetDatabase,
        QStringLiteral("Deleting and remaking database storage at %1…").arg(activeRoot_),
        []() {
            std::string error;
            const bool ok = simcore::db::DBService::instance().reset_database_root(error);
            return StorageResult{ ok, ok ? QString() : QString::fromStdString(error) };
        });
}

void SettingsPage::handleUseExistingDatabaseClicked()
{
    const QString startDir = activeRoot_.isEmpty() ? QDir::homePath() : activeRoot_;
    const QString selectedDir = QFileDialog::getExistingDirectory(this, "Select existing database root", startDir);
    if (selectedDir.isEmpty()) {
        return;
    }

    const QString targetRoot = normalizePath(selectedDir);
    if (targetRoot.isEmpty()) {
        setStatus(StatusKind::Warning, "Select a valid database root directory.");
        return;
    }
    if (targetRoot == activeRoot_) {
        setStatus(StatusKind::Info, "Selected directory is already the active database root.");
        return;
    }

    const QFileInfo dbFile(QDir(targetRoot).filePath(QStringLiteral("SoaSimDB.sqlite3")));
    if (!dbFile.exists() || !dbFile.isFile()) {
        setStatus(StatusKind::Warning, "The selected directory does not contain SoaSimDB.sqlite3.");
        return;
    }

    const auto answer = QMessageBox::question(
        this,
        "Use Existing Database",
        QStringLiteral("Switch SoaSimQt to another existing database root?\n\nCurrent root:\n%1\n\nExisting root:\n%2\n\nNo data will be copied.")
            .arg(activeRoot_, targetRoot),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }

    startStorageOperation(
        StorageOperation::UseExistingDatabase,
        QStringLiteral("Switching active database root to %1…").arg(targetRoot),
        [target = targetRoot.toStdString()]() {
            std::string error;
            const bool ok = simcore::db::DBService::instance().switch_database_root(target, error);
            return StorageResult{ ok, ok ? QString() : QString::fromStdString(error) };
        });
}

void SettingsPage::handleSaveSnapshotClicked()
{
    const QString defaultPath = QDir(activeRoot_.isEmpty() ? QDir::homePath() : activeRoot_)
        .filePath(QStringLiteral("SoaSimSnapshot.soasnap"));
    const QString snapshotPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save Database Snapshot"),
        defaultPath,
        QStringLiteral("SoaSim Snapshot (*.soasnap);;All Files (*)"));
    if (snapshotPath.isEmpty()) {
        return;
    }

    const QString normalizedSnapshotPath = normalizePath(snapshotPath);
    if (normalizedSnapshotPath.isEmpty()) {
        setStatus(StatusKind::Warning, "Select a valid snapshot file path.");
        return;
    }

    startStorageOperation(
        StorageOperation::SaveSnapshot,
        QStringLiteral("Saving database snapshot to %1…").arg(normalizedSnapshotPath),
        [path = normalizedSnapshotPath.toStdString()]() {
            const auto result = simcore::db::DbSnapshotService::SaveSnapshot(path);
            return StorageResult{ result.ok, result.ok ? QString() : QString::fromStdString(result.error) };
        });
}

void SettingsPage::handleLoadSnapshotClicked()
{
    const QString snapshotPath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Load Database Snapshot"),
        activeRoot_.isEmpty() ? QDir::homePath() : activeRoot_,
        QStringLiteral("SoaSim Snapshot (*.soasnap);;All Files (*)"));
    if (snapshotPath.isEmpty()) {
        return;
    }

    const QString targetRoot = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select target database root"),
        activeRoot_.isEmpty() ? QDir::homePath() : activeRoot_);
    if (targetRoot.isEmpty()) {
        return;
    }

    const QString normalizedTargetRoot = normalizePath(targetRoot);
    const QString normalizedSnapshotPath = normalizePath(snapshotPath);
    if (normalizedTargetRoot.isEmpty() || normalizedSnapshotPath.isEmpty()) {
        setStatus(StatusKind::Warning, "Select a valid snapshot file and target root.");
        return;
    }

    const bool overwritingCurrentRoot = normalizedTargetRoot == activeRoot_;
    const QString prompt = overwritingCurrentRoot
        ? QStringLiteral("Load this snapshot into the current active database root?\n\nSnapshot:\n%1\n\nTarget root:\n%2\n\nThe current database and artifact files in that root will be overwritten.")
        : QStringLiteral("Load this snapshot into the selected target root and switch SoaSimQt to it?\n\nSnapshot:\n%1\n\nTarget root:\n%2\n\nAny existing database or artifact files in that root may be overwritten.");

    const auto answer = QMessageBox::question(
        this,
        QStringLiteral("Load Snapshot"),
        prompt.arg(normalizedSnapshotPath, normalizedTargetRoot),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }

    startStorageOperation(
        StorageOperation::LoadSnapshot,
        QStringLiteral("Loading database snapshot into %1…").arg(normalizedTargetRoot),
        [snapshot = normalizedSnapshotPath.toStdString(), target = normalizedTargetRoot.toStdString()]() {
            const auto result = simcore::db::DbSnapshotService::LoadSnapshot(snapshot, target, true);
            return StorageResult{ result.ok, result.ok ? QString() : QString::fromStdString(result.error) };
        });
}

void SettingsPage::handleReconcileExplorerRunsClicked()
{
    if (storageBusy_ || reconcileBusy_) {
        return;
    }

    const auto answer = QMessageBox::question(
        this,
        "Backfill Explorer Runs",
        QStringLiteral("Scan historical Explorer/BattleSingleTurn runner roots and backfill missing explorer_run rows?\n\nThis is safe to run multiple times."),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }

    reconcileBusy_ = true;
    refreshStorageUi();
    setStatus(StatusKind::Working, QStringLiteral("Reconciling explorer_run backfill across existing root job sets…"));
    QCoreApplication::processEvents();
    reconcileWatcher_.setFuture(runAsync([]() {
        return simcore::db::DataService::ReconcileMissingExplorerRunsAsync().get();
    }));
}

void SettingsPage::startStorageOperation(StorageOperation op, const QString& workingMessage, StorageTask task)
{
    if (storageBusy_) {
        return;
    }

    storageBusy_ = true;
    currentStorageOperation_ = op;
    refreshStorageUi();
    setStatus(StatusKind::Working, workingMessage);
    QCoreApplication::processEvents();
    storageWatcher_.setFuture(runAsync(std::move(task)));
}

void SettingsPage::handleStorageOperationFinished()
{
    storageBusy_ = false;
    const StorageOperation completedOp = currentStorageOperation_;
    currentStorageOperation_ = StorageOperation::None;

    StorageResult result{ false, QStringLiteral("Unknown storage operation failure.") };
    try {
        result = storageWatcher_.result();
    } catch (const std::exception& ex) {
        result = StorageResult{ false, QString::fromUtf8(ex.what()) };
    } catch (...) {
        result = StorageResult{ false, QStringLiteral("Unknown exception during storage operation.") };
    }

    refreshStorageUi();
    if (!result.first) {
        setStatus(StatusKind::Failure, result.second.isEmpty() ? QStringLiteral("The storage operation failed.") : result.second);
        return;
    }

    refreshActiveRoot();

    switch (completedOp) {
    case StorageOperation::MoveDatabase:
    case StorageOperation::ResetDatabase:
    case StorageOperation::UseExistingDatabase:
    case StorageOperation::LoadSnapshot: {
        QSettings settings;
        settings.beginGroup(kSettingsGroup);
        settings.setValue(kDbRootKey, activeRoot_);
        settings.endGroup();
        persistedRoot_ = activeRoot_;
        break;
    }
    case StorageOperation::SaveSnapshot:
    case StorageOperation::None:
        break;
    }

    switch (completedOp) {
    case StorageOperation::MoveDatabase:
        setStatus(StatusKind::Success, QStringLiteral("Database storage moved successfully. Active root: %1").arg(activeRoot_));
        break;
    case StorageOperation::ResetDatabase:
        setStatus(StatusKind::Success, QStringLiteral("Database storage was deleted and recreated successfully. Active root: %1").arg(activeRoot_));
        break;
    case StorageOperation::UseExistingDatabase:
        setStatus(StatusKind::Success, QStringLiteral("Switched to the selected existing database root: %1").arg(activeRoot_));
        break;
    case StorageOperation::SaveSnapshot:
        setStatus(StatusKind::Success, QStringLiteral("Database snapshot saved successfully."));
        break;
    case StorageOperation::LoadSnapshot:
        setStatus(StatusKind::Success, QStringLiteral("Database snapshot loaded successfully. Active root: %1").arg(activeRoot_));
        break;
    case StorageOperation::None:
        break;
    }
}

void SettingsPage::handleReconcileOperationFinished()
{
    reconcileBusy_ = false;
    refreshStorageUi();

    simcore::db::DbResult<simcore::db::ExplorerRunReconcileResult> result
        = simcore::db::DbResult<simcore::db::ExplorerRunReconcileResult>::Err({ simcore::db::DbErrorKind::Unknown, 0, "Unknown reconcile failure." });
    try {
        result = reconcileWatcher_.result();
    } catch (const std::exception& ex) {
        setStatus(StatusKind::Failure, QStringLiteral("Explorer run reconcile failed: %1").arg(QString::fromUtf8(ex.what())));
        return;
    } catch (...) {
        setStatus(StatusKind::Failure, QStringLiteral("Explorer run reconcile failed with an unknown exception."));
        return;
    }

    if (!result.ok) {
        setStatus(StatusKind::Failure, QStringLiteral("Explorer run reconcile failed: %1").arg(QString::fromStdString(result.error.message)));
        return;
    }

    const auto& stats = result.value;
    setStatus(
        StatusKind::Success,
        QStringLiteral("Explorer runs reconciled. Roots scanned: %1 · roots with existing runs: %2 · roots backfilled: %3 · runs created: %4 · jobs updated: %5")
            .arg(stats.roots_scanned)
            .arg(stats.roots_with_existing_runs)
            .arg(stats.roots_reconciled)
            .arg(stats.runs_created)
            .arg(stats.jobs_updated));
}

void SettingsPage::refreshCoordinatorUi()
{
    if (!coordinatorController_) {
        return;
    }

    {
        const QSignalBlocker blocker(isoPathEdit_);
        isoPathEdit_->setText(coordinatorController_->isoPath());
    }
    {
        const QSignalBlocker blocker(dolphinBaseDirEdit_);
        dolphinBaseDirEdit_->setText(coordinatorController_->dolphinBaseDir());
    }
    {
        const QSignalBlocker blocker(eventBufferSpin_);
        eventBufferSpin_->setValue(coordinatorController_->eventBufferCapacity());
    }
    {
        const QSignalBlocker blocker(startPausedCheck_);
        startPausedCheck_->setChecked(coordinatorController_->startPaused());
    }
    {
        const QSignalBlocker blocker(requeueFailuresAutomaticallyCheck_);
        requeueFailuresAutomaticallyCheck_->setChecked(coordinatorController_->restartFailedJobsAutomatically());
    }

    const bool running = coordinatorController_->isRunning();
    isoPathEdit_->setEnabled(!running);
    isoBrowseButton_->setEnabled(!running);
    dolphinBaseDirEdit_->setEnabled(!running);
    dolphinBrowseButton_->setEnabled(!running);
    eventBufferSpin_->setEnabled(!running);
    startPausedCheck_->setEnabled(!running);
    requeueFailuresAutomaticallyCheck_->setEnabled(!running);

    coordinatorValidationLabel_->setText(
        coordinatorController_->validationMessage().isEmpty()
            ? QStringLiteral("Configuration looks good. You can start the coordinator from the Workers page when ready.")
            : coordinatorController_->validationMessage());
    coordinatorValidationLabel_->setProperty(
        "validationState",
        coordinatorController_->validationMessage().isEmpty() ? QStringLiteral("ok") : QStringLiteral("warn"));
    coordinatorValidationLabel_->style()->unpolish(coordinatorValidationLabel_);
    coordinatorValidationLabel_->style()->polish(coordinatorValidationLabel_);
}

void SettingsPage::browseForIsoPath()
{
    const QString initialPath = isoPathEdit_->text().trimmed();
    const QString selectedPath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Select Skies of Arcadia ISO"),
        initialPath);

    if (!selectedPath.isEmpty()) {
        isoPathEdit_->setText(selectedPath);
    }
}

void SettingsPage::browseForDolphinBaseDir()
{
    const QString initialPath = dolphinBaseDirEdit_->text().trimmed();
    const QString selectedDir = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select Dolphin base directory"),
        initialPath);

    if (!selectedDir.isEmpty()) {
        dolphinBaseDirEdit_->setText(selectedDir);
    }
}

void SettingsPage::setStatus(StatusKind kind, const QString& message)
{
    statusLabel_->setProperty("statusKind", statusKindToString(kind));
    statusLabel_->style()->unpolish(statusLabel_);
    statusLabel_->style()->polish(statusLabel_);
    statusLabel_->setText(message);

    StatusToast::Severity severity = StatusToast::Severity::Info;
    switch (kind) {
    case StatusKind::Warning:
        severity = StatusToast::Severity::Warn;
        break;
    case StatusKind::Success:
        severity = StatusToast::Severity::Success;
        break;
    case StatusKind::Failure:
        severity = StatusToast::Severity::Error;
        break;
    case StatusKind::Working:
    case StatusKind::Info:
        severity = StatusToast::Severity::Info;
        break;
    }

    if (!message.isEmpty()) {
        const QString signature = QStringLiteral("%1|%2").arg(static_cast<int>(severity)).arg(message);
        if (signature != lastToastSignature_) {
            lastToastSignature_ = signature;
            emit statusToastRequested(StatusToast{ severity, message, QString(), 1, QDateTime{}, 4000 });
        }
    }
}

QString SettingsPage::normalizePath(const QString& path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }

    const std::filesystem::path fsPath = std::filesystem::path(trimmed.toStdWString()).lexically_normal();
    return QDir::toNativeSeparators(QString::fromStdWString(fsPath.wstring()));
}
