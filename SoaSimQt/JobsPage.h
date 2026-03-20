#pragma once

#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <vector>

class ArtifactsTableModel;
class ArtifactsTableView;
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
class QTimer;

class JobsPage final : public QWidget
{
public:
    struct MockJob;

    explicit JobsPage(QWidget* parent = nullptr);

private:
    void buildMockJobs();
    void createWidgets();
    void wireSignals();
    void syncControlsToState();
    void applyFilters();
    void populateTable();
    void updatePageControls();
    void syncInspectorAfterFilter();
    void updateInspector(const MockJob& job);
    void clearInspector();
    int visibleRowForJob(qint64 jobId) const;
    const MockJob* selectedJob() const;
    MockJob* selectedJobMutable();
    void refreshMockProgress();
    QString timestampPrefix() const;
    QTextEdit* createReadOnlyTextEdit();

    std::vector<MockJob> allJobs_;
    std::vector<const MockJob*> filteredJobs_;

    QString selectedProgramKind_;
    QString selectedState_;
    QString selectedJobSetId_;
    bool autoRefreshEnabled_ = true;
    int refreshSeconds_ = 2;
    int pageStartIndex_ = 0;
    qint64 selectedJobId_ = 0;

    QLabel* titleLabel_ = nullptr;
    QLabel* descriptionLabel_ = nullptr;

    QComboBox* kindFilter_ = nullptr;
    QComboBox* stateFilter_ = nullptr;
    QLineEdit* jobSetFilter_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QCheckBox* autoRefreshCheck_ = nullptr;
    QSpinBox* refreshSecondsSpin_ = nullptr;

    QPushButton* prevButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QLabel* pageSummaryLabel_ = nullptr;
    QLabel* lastRefreshLabel_ = nullptr;

    JobsTableView* jobsTable_ = nullptr;
    JobsTableModel* jobsModel_ = nullptr;

    QLabel* inspectorSummary_ = nullptr;
    QPushButton* requeueButton_ = nullptr;
    QPushButton* restartButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QSpinBox* bumpDeltaSpin_ = nullptr;
    QPushButton* applyBumpButton_ = nullptr;
    QPushButton* inspectorRefreshButton_ = nullptr;
    QTabWidget* inspectorTabs_ = nullptr;

    QLabel* overviewPriorityValue_ = nullptr;
    QLabel* overviewQueuedValue_ = nullptr;
    QLabel* overviewSelectionHint_ = nullptr;
    QTextEdit* eventsText_ = nullptr;
    QTextEdit* progressText_ = nullptr;
    QTextEdit* payloadText_ = nullptr;
    QTextEdit* resultsText_ = nullptr;
    ArtifactsTableView* artifactsTable_ = nullptr;
    ArtifactsTableModel* artifactsModel_ = nullptr;

    QTimer* refreshTimer_ = nullptr;
};
