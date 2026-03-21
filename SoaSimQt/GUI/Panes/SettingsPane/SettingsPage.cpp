#include "SettingsPage.h"

#include "DB/DBCore/DbService.h"
#include "GUI/Panes/CoordinatorPane/CoordinatorController.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <filesystem>
#include <string>

namespace {
constexpr auto kSettingsGroup = "Settings";
constexpr auto kDbRootKey = "db_root";

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
    createWidgets();
    loadState();
    refreshValidation();
    refreshCoordinatorUi();

    if (coordinatorController_) {
        connect(coordinatorController_, &CoordinatorController::stateChanged, this, &SettingsPage::refreshCoordinatorUi);
    }
}

void SettingsPage::createWidgets()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(12);

    titleLabel_ = new QLabel("Settings", this);
    titleLabel_->setObjectName("pageTitle");
    rootLayout->addWidget(titleLabel_);

    descriptionLabel_ = new QLabel(
        "Application-wide configuration for storage, coordinator defaults, and future environment preferences.",
        this);
    descriptionLabel_->setObjectName("pageDescription");
    descriptionLabel_->setWordWrap(true);
    rootLayout->addWidget(descriptionLabel_);

    const CollapsibleSection storageSection = createCollapsibleSection(
        "SETTINGS SECTION",
        "Database Storage",
        "Move the SoaSim database root using the same backend relocation flow as SoaSimGui. This stops DB services, copies existing DB/object storage data, and restarts using the new root.");

    QVBoxLayout* storageLayout = new QVBoxLayout(storageSection.content);
    storageLayout->setContentsMargins(0, 0, 0, 0);
    storageLayout->setSpacing(14);

    QGridLayout* formLayout = new QGridLayout();
    formLayout->setHorizontalSpacing(12);
    formLayout->setVerticalSpacing(10);

    QLabel* activeRootLabel = new QLabel("Active database root", storageSection.content);
    activeRootLabel->setObjectName("settingsFieldLabel");
    activeRootValueLabel_ = new QLabel(storageSection.content);
    activeRootValueLabel_->setObjectName("settingsValueLabel");
    activeRootValueLabel_->setWordWrap(true);

    QLabel* targetRootLabel = new QLabel("New database root", storageSection.content);
    targetRootLabel->setObjectName("settingsFieldLabel");
    dbRootEdit_ = new QLineEdit(storageSection.content);
    dbRootEdit_->setObjectName("settingsPathEdit");
    dbRootEdit_->setPlaceholderText("Example: D:/SOASimData");
    connect(dbRootEdit_, &QLineEdit::textChanged, this, &SettingsPage::handleInputChanged);

    browseButton_ = new QPushButton("Browse…", storageSection.content);
    browseButton_->setObjectName("jobsSecondaryButton");
    connect(browseButton_, &QPushButton::clicked, this, &SettingsPage::handleBrowseClicked);

    formLayout->addWidget(activeRootLabel, 0, 0);
    formLayout->addWidget(activeRootValueLabel_, 0, 1, 1, 2);
    formLayout->addWidget(targetRootLabel, 1, 0);
    formLayout->addWidget(dbRootEdit_, 1, 1);
    formLayout->addWidget(browseButton_, 1, 2);
    formLayout->setColumnStretch(1, 1);
    storageLayout->addLayout(formLayout);

    validationLabel_ = new QLabel(storageSection.content);
    validationLabel_->setObjectName("settingsValidation");
    validationLabel_->setWordWrap(true);
    storageLayout->addWidget(validationLabel_);

    statusLabel_ = new QLabel(storageSection.content);
    statusLabel_->setObjectName("settingsStatus");
    statusLabel_->setWordWrap(true);
    storageLayout->addWidget(statusLabel_);

    QHBoxLayout* actionLayout = new QHBoxLayout();
    actionLayout->setSpacing(10);
    actionLayout->addStretch();

    applyButton_ = new QPushButton("Apply and Move Data", storageSection.content);
    applyButton_->setObjectName("jobsPrimaryButton");
    connect(applyButton_, &QPushButton::clicked, this, &SettingsPage::handleApplyClicked);
    actionLayout->addWidget(applyButton_);

    storageLayout->addLayout(actionLayout);
    rootLayout->addWidget(storageSection.card);

    const CollapsibleSection coordinatorSection = createCollapsibleSection(
        "RUNTIME SECTION",
        "Coordinator",
        "Coordinator startup defaults now live here so the Workers page can focus on runtime control and telemetry.");

    QVBoxLayout* coordinatorLayout = new QVBoxLayout(coordinatorSection.content);
    coordinatorLayout->setContentsMargins(0, 0, 0, 0);
    coordinatorLayout->setSpacing(14);

    QGridLayout* coordinatorFormLayout = new QGridLayout();
    coordinatorFormLayout->setHorizontalSpacing(12);
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
    }
    connect(isoBrowseButton_, &QPushButton::clicked, this, &SettingsPage::browseForIsoPath);
    connect(dolphinBrowseButton_, &QPushButton::clicked, this, &SettingsPage::browseForDolphinBaseDir);

    rootLayout->addWidget(coordinatorSection.card);

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
    cardLayout->setContentsMargins(18, 18, 18, 18);
    cardLayout->setSpacing(12);

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

    const QString initialRoot = persistedRoot_.isEmpty() ? activeRoot_ : persistedRoot_;
    dbRootEdit_->setText(initialRoot);

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

void SettingsPage::handleBrowseClicked()
{
    const QString startDir = normalizedInput_.isEmpty() ? activeRoot_ : normalizedInput_;
    const QString selectedDir = QFileDialog::getExistingDirectory(this, "Select database root", startDir);
    if (!selectedDir.isEmpty()) {
        dbRootEdit_->setText(QDir::toNativeSeparators(selectedDir));
    }
}

void SettingsPage::handleApplyClicked()
{
    refreshValidation();
    if (!canApply_) {
        return;
    }

    const QString targetRoot = normalizedInput_;
    setStatus(StatusKind::Working, "Relocating database storage. Please wait…");
    QCoreApplication::processEvents();

    std::string error;
    const bool ok = simcore::db::DBService::instance().relocate_database_root(targetRoot.toStdString(), error);
    if (!ok) {
        setStatus(StatusKind::Failure,
            error.empty()
                ? "The shared database relocation backend reported a failure."
                : QString::fromStdString(error));
        refreshActiveRoot();
        refreshValidation();
        return;
    }

    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    settings.setValue(kDbRootKey, targetRoot);
    settings.endGroup();
    persistedRoot_ = targetRoot;

    refreshActiveRoot();
    dbRootEdit_->setText(activeRoot_);
    setStatus(StatusKind::Success, "Database storage moved successfully and the new root was saved for future SoaSimQt launches.");
    refreshValidation();
}

void SettingsPage::handleInputChanged()
{
    refreshValidation();
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

    const bool running = coordinatorController_->isRunning();
    isoPathEdit_->setEnabled(!running);
    isoBrowseButton_->setEnabled(!running);
    dolphinBaseDirEdit_->setEnabled(!running);
    dolphinBrowseButton_->setEnabled(!running);
    eventBufferSpin_->setEnabled(!running);
    startPausedCheck_->setEnabled(!running);

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

void SettingsPage::refreshValidation()
{
    refreshActiveRoot();
    normalizedInput_ = normalizePath(dbRootEdit_->text());

    QString validationMessage;
    QString validationState = "invalid";
    canApply_ = false;

    if (normalizedInput_.isEmpty()) {
        validationMessage = "Enter a target directory for the database root.";
    } else if (normalizedInput_ == activeRoot_) {
        validationMessage = "Choose a different directory than the current active database root.";
    } else {
        QFileInfo inputInfo(normalizedInput_);
        const QFileInfo parentInfo(inputInfo.dir().absolutePath());
        if (inputInfo.exists() && !inputInfo.isDir()) {
            validationMessage = "The selected path exists but is not a directory.";
        } else if (!parentInfo.exists() || !parentInfo.isDir()) {
            validationMessage = "The parent directory does not exist or is not reachable.";
        } else {
            validationMessage = "Ready to relocate the database root using the shared backend flow.";
            validationState = "valid";
            canApply_ = true;
        }
    }

    validationLabel_->setProperty("validationState", validationState);
    validationLabel_->style()->unpolish(validationLabel_);
    validationLabel_->style()->polish(validationLabel_);
    validationLabel_->setText(validationMessage);
    applyButton_->setEnabled(canApply_);
}

void SettingsPage::setStatus(StatusKind kind, const QString& message)
{
    statusLabel_->setProperty("statusKind", statusKindToString(kind));
    statusLabel_->style()->unpolish(statusLabel_);
    statusLabel_->style()->polish(statusLabel_);
    statusLabel_->setText(message);
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
