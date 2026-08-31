#pragma once

#include <QtCore/QFutureWatcher>
#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"
#include "GUI/Refresh/ViewState.h"

#include <functional>
#include <utility>

class CoordinatorController;
class QLabel;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QFrame;
class QToolButton;
class QWidget;

typedef std::pair<bool, QString> SettingsStorageResult;

class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    enum class CoordinatorFocusTarget {
        Section,
        IsoPath,
        DolphinBaseDir
    };

    explicit SettingsPage(CoordinatorController* coordinatorController, QWidget* parent = nullptr);
    void focusCoordinatorSettings(CoordinatorFocusTarget target);

    enum class StatusKind {
        Info,
        Warning,
        Success,
        Failure,
        Working
    };

signals:
    void statusToastRequested(StatusToast toast);

private slots:
    void refreshCoordinatorUi();
    void browseForIsoPath();
    void browseForDolphinBaseDir();
    void handleMoveDatabaseClicked();
    void handleResetDatabaseClicked();
    void handleResetResultStagingClicked();
    void handleUseExistingDatabaseClicked();
    void handleSaveSnapshotClicked();
    void handleLoadSnapshotClicked();
    void handleStorageOperationFinished();

private:
    struct CollapsibleSection {
        QFrame* card = nullptr;
        QToolButton* toggleButton = nullptr;
        QWidget* content = nullptr;
    };

    enum class StorageOperation {
        None,
        MoveDatabase,
        ResetDatabase,
        ResetResultStaging,
        UseExistingDatabase,
        SaveSnapshot,
        LoadSnapshot,
    };

    using StorageTask = std::function<SettingsStorageResult()>;

    void createWidgets();
    CollapsibleSection createCollapsibleSection(const QString& eyebrow, const QString& title, const QString& description, bool expandedByDefault = true);
    void loadState();
    void refreshActiveRoot();
    void refreshStorageUi();
    void setStatus(StatusKind kind, const QString& message);
    void startStorageOperation(StorageOperation op, const QString& workingMessage, StorageTask task);
    static QString normalizePath(const QString& path);

    CoordinatorController* coordinatorController_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* descriptionLabel_ = nullptr;
    QLabel* activeRootValueLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* moveDatabaseButton_ = nullptr;
    QPushButton* resetDatabaseButton_ = nullptr;
    QPushButton* resetResultStagingButton_ = nullptr;
    QPushButton* useExistingButton_ = nullptr;
    QPushButton* saveSnapshotButton_ = nullptr;
    QPushButton* loadSnapshotButton_ = nullptr;

    QLineEdit* isoPathEdit_ = nullptr;
    QPushButton* isoBrowseButton_ = nullptr;
    QLineEdit* dolphinBaseDirEdit_ = nullptr;
    QPushButton* dolphinBrowseButton_ = nullptr;
    QCheckBox* startPausedCheck_ = nullptr;
    savorqt::gui::DraftState<QString> isoPathDraft_;
    savorqt::gui::DraftState<QString> dolphinBaseDirDraft_;
    QLabel* coordinatorValidationLabel_ = nullptr;
    QToolButton* coordinatorSectionToggle_ = nullptr;

    QString activeRoot_;
    QString persistedRoot_;
    bool storageBusy_ = false;
    StorageOperation currentStorageOperation_ = StorageOperation::None;
    QFutureWatcher<SettingsStorageResult> storageWatcher_;
    QString lastToastSignature_;
};
