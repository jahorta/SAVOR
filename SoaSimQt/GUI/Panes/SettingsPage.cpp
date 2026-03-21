#include "SettingsPage.h"

#include "DB/DBCore/DbService.h"

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

namespace {
constexpr auto kSettingsGroup = "Settings";
constexpr auto kDbRootKey = "db_root";
}

SettingsPage::SettingsPage(QWidget* parent)
    : QWidget(parent)
{
    createWidgets();
    loadPersistedState();
    refreshUi();
}

void SettingsPage::browseForDatabaseRoot()
{
    const QString initialPath = databaseRootEdit_->text().trimmed().isEmpty()
        ? currentDatabaseRoot_
        : databaseRootEdit_->text().trimmed();
    const QString selectedDir = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select database root"),
        initialPath);

    if (!selectedDir.isEmpty()) {
        databaseRootEdit_->setText(QDir::toNativeSeparators(selectedDir));
    }
}

void SettingsPage::applyDatabaseRootChange()
{
    const QString target = normalizedPath(databaseRootEdit_->text());
    if (target.isEmpty()) {
        updateStatusMessage(QStringLiteral("Database root cannot be empty."), QStringLiteral("error"));
        refreshUi();
        return;
    }

    updateStatusMessage(
        QStringLiteral("Moving database files and restarting database services…"),
        QStringLiteral("working"));
    applyButton_->setEnabled(false);

    std::string error;
    const bool ok = simcore::db::DBService::instance().relocate_database_root(target.toStdString(), error);
    if (!ok) {
        const QString message = error.empty()
            ? QStringLiteral("Failed to relocate database root.")
            : QString::fromStdString(error);
        updateStatusMessage(message, QStringLiteral("error"));
        refreshUi();
        return;
    }

    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    settings.setValue(kDbRootKey, target);
    settings.endGroup();

    currentDatabaseRoot_ = QDir::toNativeSeparators(
        QString::fromStdString(simcore::db::DBService::instance().database_root().string()));
    databaseRootEdit_->setText(QDir::toNativeSeparators(target));
    updateStatusMessage(QStringLiteral("Database storage moved successfully."), QStringLiteral("success"));
    refreshUi();
}

void SettingsPage::refreshUi()
{
    currentDatabaseRoot_ = QDir::toNativeSeparators(
        QString::fromStdString(simcore::db::DBService::instance().database_root().string()));
    currentDatabaseRootValueLabel_->setText(currentDatabaseRoot_.isEmpty()
        ? QStringLiteral("--")
        : currentDatabaseRoot_);

    const QString target = normalizedPath(databaseRootEdit_->text());
    const QString current = normalizedPath(currentDatabaseRoot_);
    const bool hasTarget = !target.isEmpty();
    const bool matchesCurrent = hasTarget && target == current;
    const QFileInfo targetInfo(target);
    const bool parentExists = !hasTarget
        ? false
        : (targetInfo.exists() ? targetInfo.isDir() : QFileInfo(targetInfo.dir().absolutePath()).exists());

    const QString state = statusLabel_->property("settingsState").toString();
    if (!hasTarget) {
        updateStatusMessage(QStringLiteral("Choose a new database root to enable Apply."), QStringLiteral("info"));
    } else if (matchesCurrent) {
        updateStatusMessage(QStringLiteral("The selected folder is already the active database root."), QStringLiteral("warn"));
    } else if (!parentExists) {
        updateStatusMessage(QStringLiteral("The selected path is not reachable yet. Pick an existing parent folder or create it first."), QStringLiteral("warn"));
    } else if (state != QStringLiteral("success")
        && state != QStringLiteral("error")
        && state != QStringLiteral("working")) {
        updateStatusMessage(QStringLiteral("Applying this change will stop database services, move existing database files and object storage, then restart."), QStringLiteral("info"));
    }

    applyButton_->setEnabled(hasTarget && !matchesCurrent && parentExists && state != QStringLiteral("working"));
}

void SettingsPage::createWidgets()
{
    rootLayout_ = new QVBoxLayout(this);
    rootLayout_->setContentsMargins(0, 0, 0, 0);
    rootLayout_->setSpacing(12);

    rootLayout_->addWidget(createDatabaseStorageSection());
    rootLayout_->addWidget(createPlaceholderSection(
        QStringLiteral("More Settings Coming Soon"),
        QStringLiteral("This page is scaffolded for future sections such as environment setup, diagnostics, and default runtime preferences.")));
    rootLayout_->addStretch();
}

QWidget* SettingsPage::createDatabaseStorageSection()
{
    QFrame* card = new QFrame(this);
    card->setObjectName("coordinatorCard");

    QVBoxLayout* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    QLabel* heading = new QLabel(QStringLiteral("Database Storage"), card);
    heading->setObjectName("panelTitle");

    QLabel* body = new QLabel(
        QStringLiteral("Use the same database-root relocation flow as SoaSimGui, with extra Qt affordances for browsing, inline validation, and in-page status feedback."),
        card);
    body->setObjectName("panelBody");
    body->setWordWrap(true);

    QGridLayout* formLayout = new QGridLayout();
    formLayout->setHorizontalSpacing(12);
    formLayout->setVerticalSpacing(10);

    currentDatabaseRootValueLabel_ = new QLabel(card);
    currentDatabaseRootValueLabel_->setObjectName("settingsCurrentValue");
    currentDatabaseRootValueLabel_->setWordWrap(true);
    currentDatabaseRootValueLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    databaseRootEdit_ = new QLineEdit(card);
    databaseRootEdit_->setPlaceholderText(QStringLiteral("Example: D:/SOASimData"));

    browseButton_ = new QPushButton(QStringLiteral("Browse…"), card);
    browseButton_->setObjectName("jobsSecondaryButton");

    QHBoxLayout* editLayout = new QHBoxLayout();
    editLayout->setContentsMargins(0, 0, 0, 0);
    editLayout->setSpacing(8);
    editLayout->addWidget(databaseRootEdit_, 1);
    editLayout->addWidget(browseButton_);

    formLayout->addWidget(createFieldCaption(QStringLiteral("Current root"), card), 0, 0);
    formLayout->addWidget(currentDatabaseRootValueLabel_, 0, 1);
    formLayout->addWidget(createFieldCaption(QStringLiteral("New root"), card), 1, 0);
    formLayout->addLayout(editLayout, 1, 1);

    QLabel* note = new QLabel(
        QStringLiteral("Changing this location will move the existing database files and object storage, then restart the database service using the new root."),
        card);
    note->setObjectName("panelBody");
    note->setWordWrap(true);

    applyButton_ = new QPushButton(QStringLiteral("Apply and Move Data"), card);
    applyButton_->setObjectName("jobsPrimaryButton");

    QHBoxLayout* actionLayout = new QHBoxLayout();
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(10);
    actionLayout->addWidget(applyButton_);
    actionLayout->addStretch();

    statusLabel_ = new QLabel(card);
    statusLabel_->setObjectName("settingsStatus");
    statusLabel_->setWordWrap(true);

    layout->addWidget(heading);
    layout->addWidget(body);
    layout->addLayout(formLayout);
    layout->addWidget(note);
    layout->addLayout(actionLayout);
    layout->addWidget(statusLabel_);

    connect(browseButton_, &QPushButton::clicked, this, &SettingsPage::browseForDatabaseRoot);
    connect(applyButton_, &QPushButton::clicked, this, &SettingsPage::applyDatabaseRootChange);
    connect(databaseRootEdit_, &QLineEdit::textChanged, this, &SettingsPage::refreshUi);

    return card;
}

QWidget* SettingsPage::createPlaceholderSection(const QString& title, const QString& description)
{
    QFrame* card = new QFrame(this);
    card->setObjectName("coordinatorCard");

    QVBoxLayout* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(8);

    QLabel* heading = new QLabel(title, card);
    heading->setObjectName("panelTitle");

    QLabel* body = new QLabel(description, card);
    body->setObjectName("panelBody");
    body->setWordWrap(true);

    layout->addWidget(heading);
    layout->addWidget(body);

    return card;
}

QLabel* SettingsPage::createFieldCaption(const QString& text, QWidget* parent) const
{
    QLabel* label = new QLabel(text, parent);
    label->setObjectName("coordinatorFieldCaption");
    return label;
}

void SettingsPage::loadPersistedState()
{
    currentDatabaseRoot_ = QDir::toNativeSeparators(
        QString::fromStdString(simcore::db::DBService::instance().database_root().string()));

    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    QString savedRoot = settings.value(kDbRootKey).toString().trimmed();
    settings.endGroup();

    if (savedRoot.isEmpty()) {
        savedRoot = currentDatabaseRoot_;
    }

    databaseRootEdit_->setText(QDir::toNativeSeparators(savedRoot));
}

void SettingsPage::updateStatusMessage(const QString& text, const QString& state)
{
    statusLabel_->setText(text);
    statusLabel_->setProperty("settingsState", state);
    statusLabel_->style()->unpolish(statusLabel_);
    statusLabel_->style()->polish(statusLabel_);
}

QString SettingsPage::normalizedPath(const QString& path) const
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    return QDir::cleanPath(QDir::fromNativeSeparators(trimmed));
}
