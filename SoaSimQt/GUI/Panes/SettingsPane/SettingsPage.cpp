#include "SettingsPage.h"

#include "DB/DBCore/DbService.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStyle>
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

SettingsPage::SettingsPage(QWidget* parent)
    : QWidget(parent)
{
    createWidgets();
    loadState();
    refreshValidation();
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
        "Application-wide configuration for storage and future environment preferences.",
        this);
    descriptionLabel_->setObjectName("pageDescription");
    descriptionLabel_->setWordWrap(true);
    rootLayout->addWidget(descriptionLabel_);

    QFrame* storageCard = new QFrame(this);
    storageCard->setObjectName("settingsCard");
    QVBoxLayout* storageLayout = new QVBoxLayout(storageCard);
    storageLayout->setContentsMargins(18, 18, 18, 18);
    storageLayout->setSpacing(14);

    QLabel* sectionEyebrow = new QLabel("SETTINGS SECTION", storageCard);
    sectionEyebrow->setObjectName("settingsSectionEyebrow");
    storageLayout->addWidget(sectionEyebrow);

    QLabel* sectionTitle = new QLabel("Database Storage", storageCard);
    sectionTitle->setObjectName("settingsSectionTitle");
    storageLayout->addWidget(sectionTitle);

    QLabel* sectionDescription = new QLabel(
        "Move the SoaSim database root using the same backend relocation flow as SoaSimGui. "
        "This stops DB services, copies existing DB/object storage data, and restarts using the new root.",
        storageCard);
    sectionDescription->setObjectName("settingsSectionDescription");
    sectionDescription->setWordWrap(true);
    storageLayout->addWidget(sectionDescription);

    QGridLayout* formLayout = new QGridLayout();
    formLayout->setHorizontalSpacing(12);
    formLayout->setVerticalSpacing(10);

    QLabel* activeRootLabel = new QLabel("Active database root", storageCard);
    activeRootLabel->setObjectName("settingsFieldLabel");
    activeRootValueLabel_ = new QLabel(storageCard);
    activeRootValueLabel_->setObjectName("settingsValueLabel");
    activeRootValueLabel_->setWordWrap(true);

    QLabel* targetRootLabel = new QLabel("New database root", storageCard);
    targetRootLabel->setObjectName("settingsFieldLabel");
    dbRootEdit_ = new QLineEdit(storageCard);
    dbRootEdit_->setObjectName("settingsPathEdit");
    dbRootEdit_->setPlaceholderText("Example: D:/SOASimData");
    connect(dbRootEdit_, &QLineEdit::textChanged, this, &SettingsPage::handleInputChanged);

    browseButton_ = new QPushButton("Browse…", storageCard);
    browseButton_->setObjectName("jobsSecondaryButton");
    connect(browseButton_, &QPushButton::clicked, this, &SettingsPage::handleBrowseClicked);

    formLayout->addWidget(activeRootLabel, 0, 0);
    formLayout->addWidget(activeRootValueLabel_, 0, 1, 1, 2);
    formLayout->addWidget(targetRootLabel, 1, 0);
    formLayout->addWidget(dbRootEdit_, 1, 1);
    formLayout->addWidget(browseButton_, 1, 2);
    formLayout->setColumnStretch(1, 1);
    storageLayout->addLayout(formLayout);

    validationLabel_ = new QLabel(storageCard);
    validationLabel_->setObjectName("settingsValidation");
    validationLabel_->setWordWrap(true);
    storageLayout->addWidget(validationLabel_);

    statusLabel_ = new QLabel(storageCard);
    statusLabel_->setObjectName("settingsStatus");
    statusLabel_->setWordWrap(true);
    storageLayout->addWidget(statusLabel_);

    QHBoxLayout* actionLayout = new QHBoxLayout();
    actionLayout->setSpacing(10);
    actionLayout->addStretch();

    applyButton_ = new QPushButton("Apply and Move Data", storageCard);
    applyButton_->setObjectName("jobsPrimaryButton");
    connect(applyButton_, &QPushButton::clicked, this, &SettingsPage::handleApplyClicked);
    actionLayout->addWidget(applyButton_);

    storageLayout->addLayout(actionLayout);
    rootLayout->addWidget(storageCard);

    QFrame* futureCard = new QFrame(this);
    futureCard->setObjectName("settingsCard");
    QVBoxLayout* futureLayout = new QVBoxLayout(futureCard);
    futureLayout->setContentsMargins(18, 18, 18, 18);
    futureLayout->setSpacing(8);

    QLabel* futureTitle = new QLabel("Future sections", futureCard);
    futureTitle->setObjectName("settingsSectionTitle");
    futureLayout->addWidget(futureTitle);

    QLabel* futureBody = new QLabel(
        "Scaffold reserved for additional SoaSimQt settings sections such as worker defaults, diagnostics, and UI preferences.",
        futureCard);
    futureBody->setObjectName("settingsSectionDescription");
    futureBody->setWordWrap(true);
    futureLayout->addWidget(futureBody);

    rootLayout->addWidget(futureCard);
    rootLayout->addStretch();
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
