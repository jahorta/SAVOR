#pragma once

#include <QtWidgets/QWidget>

#include <optional>

class QPoint;

class ArtifactsTableModel;
class ArtifactsTableView;
class JobsController;
class JobsTableModel;
class JobsTableView;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTextEdit;

class JobsPage final : public QWidget
{
public:
    explicit JobsPage(QWidget* parent = nullptr);

private:
    void createWidgets();
    void wireSignals();
    void syncControlsFromController();
    void refreshModel();
    void updateInspector();
    void updateStatusWidgets();
    QTextEdit* createReadOnlyTextEdit();
    std::optional<int> selectedProgramKind() const;
    std::optional<QString> selectedState() const;
    std::optional<qint64> selectedJobSetId() const;
    void handleRestartRequested();
    void showJobsContextMenu(const QPoint& position);

    JobsController* controller_ = nullptr;
    JobsTableModel* jobsModel_ = nullptr;
    ArtifactsTableModel* artifactsModel_ = nullptr;

    QLabel* titleLabel_ = nullptr;
    QLabel* descriptionLabel_ = nullptr;
    QComboBox* kindFilter_ = nullptr;
    QComboBox* stateFilter_ = nullptr;
    QLineEdit* jobSetFilter_ = nullptr;
    QSpinBox* pageSizeSpin_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QCheckBox* autoRefreshCheck_ = nullptr;
    QSpinBox* refreshSecondsSpin_ = nullptr;
    QPushButton* prevButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QLabel* pageSummaryLabel_ = nullptr;
    QLabel* lastRefreshLabel_ = nullptr;
    QLabel* inlineMessageLabel_ = nullptr;

    JobsTableView* jobsTable_ = nullptr;
    QLabel* inspectorSummary_ = nullptr;
    QTabWidget* inspectorTabs_ = nullptr;
    QLabel* overviewPriorityValue_ = nullptr;
    QLabel* overviewQueuedValue_ = nullptr;
    QLabel* overviewSelectionHint_ = nullptr;
    QTextEdit* eventsText_ = nullptr;
    QTextEdit* progressText_ = nullptr;
    QTextEdit* payloadText_ = nullptr;
    QTextEdit* resultsText_ = nullptr;
    ArtifactsTableView* artifactsTable_ = nullptr;
};
