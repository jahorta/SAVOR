#pragma once

#include <QtCore/QString>
#include <QtWidgets/QWidget>

class CoordinatorController;
class QLabel;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QFrame;
class QSpinBox;
class QToolButton;
class QWidget;

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

private slots:
    void handleBrowseClicked();
    void handleApplyClicked();
    void handleInputChanged();
    void refreshCoordinatorUi();
    void browseForIsoPath();
    void browseForDolphinBaseDir();

private:
    struct CollapsibleSection {
        QFrame* card = nullptr;
        QToolButton* toggleButton = nullptr;
        QWidget* content = nullptr;
    };

    void createWidgets();
    CollapsibleSection createCollapsibleSection(const QString& eyebrow, const QString& title, const QString& description, bool expandedByDefault = true);
    void loadState();
    void refreshActiveRoot();
    void refreshValidation();
    void setStatus(StatusKind kind, const QString& message);
    static QString normalizePath(const QString& path);

    CoordinatorController* coordinatorController_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* descriptionLabel_ = nullptr;
    QLabel* activeRootValueLabel_ = nullptr;
    QLineEdit* dbRootEdit_ = nullptr;
    QLabel* validationLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QPushButton* applyButton_ = nullptr;

    QLineEdit* isoPathEdit_ = nullptr;
    QPushButton* isoBrowseButton_ = nullptr;
    QLineEdit* dolphinBaseDirEdit_ = nullptr;
    QPushButton* dolphinBrowseButton_ = nullptr;
    QSpinBox* eventBufferSpin_ = nullptr;
    QCheckBox* startPausedCheck_ = nullptr;
    QLabel* coordinatorValidationLabel_ = nullptr;
    QToolButton* coordinatorSectionToggle_ = nullptr;

    QString activeRoot_;
    QString persistedRoot_;
    QString normalizedInput_;
    bool canApply_ = false;
};
