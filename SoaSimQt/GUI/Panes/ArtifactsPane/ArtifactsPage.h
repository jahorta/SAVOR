#pragma once

#include <QtWidgets/QWidget>

class ArtifactsController;
class ArtifactsBrowserTableModel;
class ArtifactsBrowserTableView;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTextEdit;

class ArtifactsPage final : public QWidget
{
    Q_OBJECT

public:
    explicit ArtifactsPage(QWidget* parent = nullptr);

private slots:
    void handleImportRequested();
    void handleExportRequested();

private:
    void createWidgets();
    void wireSignals();
    void syncControlsFromController();
    void refreshModel();
    void updateInspector();
    void updateStatusWidgets();

    ArtifactsController* controller_ = nullptr;
    ArtifactsBrowserTableModel* tableModel_ = nullptr;

    QLabel* titleLabel_ = nullptr;
    QLabel* descriptionLabel_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QLineEdit* extensionEdit_ = nullptr;
    QSpinBox* pageSizeSpin_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QPushButton* importButton_ = nullptr;
    QPushButton* prevButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QLabel* pageSummaryLabel_ = nullptr;
    QLabel* lastRefreshLabel_ = nullptr;
    QLabel* inlineMessageLabel_ = nullptr;

    ArtifactsBrowserTableView* artifactsTable_ = nullptr;
    QLabel* rootsBannerLabel_ = nullptr;
    QLabel* inspectorSummary_ = nullptr;
    QLabel* inspectorIdValue_ = nullptr;
    QLabel* inspectorFilenameValue_ = nullptr;
    QLabel* inspectorSizeValue_ = nullptr;
    QLabel* inspectorCompressionValue_ = nullptr;
    QLabel* inspectorCreatedValue_ = nullptr;
    QTextEdit* inspectorShaText_ = nullptr;
    QPushButton* exportButton_ = nullptr;
};
