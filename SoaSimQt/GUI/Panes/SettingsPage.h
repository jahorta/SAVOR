#pragma once

#include <QtWidgets/QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QString;
class QVBoxLayout;

class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPage(QWidget* parent = nullptr);

private slots:
    void browseForDatabaseRoot();
    void applyDatabaseRootChange();
    void refreshUi();

private:
    void createWidgets();
    QWidget* createDatabaseStorageSection();
    QWidget* createPlaceholderSection(const QString& title, const QString& description);
    QLabel* createFieldCaption(const QString& text, QWidget* parent) const;
    void loadPersistedState();
    void updateStatusMessage(const QString& text, const QString& state);
    QString normalizedPath(const QString& path) const;

    QVBoxLayout* rootLayout_ = nullptr;
    QLabel* currentDatabaseRootValueLabel_ = nullptr;
    QLineEdit* databaseRootEdit_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QString currentDatabaseRoot_;
};
