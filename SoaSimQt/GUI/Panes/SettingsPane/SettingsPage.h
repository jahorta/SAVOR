#pragma once

#include <QtCore/QString>
#include <QtWidgets/QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPage(QWidget* parent = nullptr);

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

private:
    void createWidgets();
    void loadState();
    void refreshActiveRoot();
    void refreshValidation();
    void setStatus(StatusKind kind, const QString& message);
    static QString normalizePath(const QString& path);

    QLabel* titleLabel_ = nullptr;
    QLabel* descriptionLabel_ = nullptr;
    QLabel* activeRootValueLabel_ = nullptr;
    QLineEdit* dbRootEdit_ = nullptr;
    QLabel* validationLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QPushButton* applyButton_ = nullptr;

    QString activeRoot_;
    QString persistedRoot_;
    QString normalizedInput_;
    bool canApply_ = false;
};
