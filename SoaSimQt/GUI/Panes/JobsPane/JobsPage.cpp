#include "JobsPage.h"

#include <QtCore/QDateTime>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtCore/QItemSelectionModel>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>

#include "ArtifactsTableModel.h"
#include "ArtifactsTableView.h"
#include "JobsTableModel.h"
#include "JobsTableView.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace {
constexpr int kJobsPageSize = 8;
} // namespace

struct JobsPage::MockJob {
    qint64 jobId = 0;
    qint64 jobSetId = 0;
    std::optional<qint64> savestateId;
    QString programKind;
    QString state;
    int attempts = 0;
    QString queuedAt;
    QString progress;
    int priority = 0;
    QStringList events;
    QString payload;
    QString results;
    QStringList artifacts;
    QStringList decodedProgress;
};

JobsPage::JobsPage(QWidget* parent)
    : QWidget(parent)
{
    buildMockJobs();
    createWidgets();
    wireSignals();
    syncControlsToState();
    applyFilters();
}

void JobsPage::buildMockJobs()
{
    allJobs_ = {
        {41021, 2101, 915, "BattleSim", "RUNNING", 1, "2026-03-20 10:12:03", "Step 183 / 600 • exploring branch 44", 12,
         {"10:12:03 CLAIMED by worker-2", "10:12:05 RUNNING", "10:14:17 PROGRESS snapshot saved"},
         "[job]\nseed=10881\nmode=BattleTower\ntrainer=Palmer\n", "[results]\noutcome=pending\n", {"trace.log (32 KB)", "battle.mp4 (4.2 MB)"},
         {"Loaded savestate 915", "Advanced to battle 183", "Win rate estimate: 61.2%"}},
        {41020, 2101, 914, "BattleSim", "FAILED", 3, "2026-03-20 10:08:22", "AI mismatch at turn 27", 7,
         {"10:08:22 CLAIMED by worker-5", "10:09:02 RUNNING", "10:09:19 FAILED: desync"},
         "[job]\nseed=10880\nmode=BattleTower\ntrainer=Palmer\n", "[results]\noutcome=failed\nturn=27\n", {"failure_dump.zip (512 KB)"},
         {"RNG advanced unexpectedly", "Expected move Protect, saw Quick Attack"}},
        {41019, 2100, std::nullopt, "SeedSearch", "QUEUED", 0, "2026-03-20 10:07:41", "Waiting for worker claim", 4,
         {"10:07:41 QUEUED"},
         "[job]\nseed_range=100000-120000\nfilters=timid,synchronize\n", "[results]\noutcome=pending\n", {},
         {"Queued for search worker"}},
        {41018, 2099, 910, "BattleSim", "SUCCEEDED", 1, "2026-03-20 10:03:55", "Completed in 00:07:18", 15,
         {"10:03:55 CLAIMED by worker-1", "10:04:03 RUNNING", "10:11:13 SUCCEEDED"},
         "[job]\nseed=10870\nmode=Factory\nround=42\n", "[results]\noutcome=winner\nscore=42\n", {"result.ini (2 KB)", "trace.log (12 KB)", "summary.png (180 KB)"},
         {"Simulation started", "Reached round 42", "Winner confirmed"}},
        {41017, 2099, 909, "BattleSim", "CANCELED", 2, "2026-03-20 10:02:41", "Canceled by operator", 6,
         {"10:02:41 CLAIMED by worker-3", "10:03:02 RUNNING", "10:03:19 CANCELED"},
         "[job]\nseed=10869\nmode=Factory\nround=41\n", "[results]\noutcome=canceled\n", {"partial_trace.log (7 KB)"},
         {"Execution interrupted by user"}},
        {41016, 2098, 907, "Explorer", "RUNNING", 1, "2026-03-20 09:58:31", "Wave 9 / 12 • 3 active leaves", 11,
         {"09:58:31 CLAIMED by worker-6", "09:58:42 RUNNING", "10:05:04 PROGRESS decoded"},
         "[job]\nwave_count=12\nbranching=aggressive\n", "[results]\noutcome=pending\n", {"explorer.dot (96 KB)"},
         {"Wave 9 started", "3 promising branches remain", "Best score so far: 188"}},
        {41015, 2097, 902, "SeedSearch", "SUCCEEDED", 1, "2026-03-20 09:44:10", "Found 12 matching seeds", 9,
         {"09:44:10 CLAIMED by worker-4", "09:44:21 RUNNING", "09:46:32 SUCCEEDED"},
         "[job]\nseed_range=70000-90000\nfilters=adamant,31atk\n", "[results]\noutcome=success\nmatches=12\n", {"matches.csv (14 KB)", "results.ini (1 KB)"},
         {"12 matches exported", "Best seed: 81234"}},
        {41014, 2097, 901, "BattleSim", "INTERRUPTED", 2, "2026-03-20 09:40:48", "Worker heartbeat lost", 5,
         {"09:40:48 CLAIMED by worker-9", "09:41:02 RUNNING", "09:43:11 INTERRUPTED"},
         "[job]\nseed=10844\nmode=BattleTower\ntrainer=Argenta\n", "[results]\noutcome=interrupted\n", {"worker-heartbeat.txt (512 B)"},
         {"Heartbeat timeout exceeded", "Eligible for requeue"}},
        {41013, 2096, 900, "Explorer", "QUEUED", 0, "2026-03-20 09:35:22", "Queued behind 4 higher-priority jobs", 3,
         {"09:35:22 QUEUED"},
         "[job]\nwave_count=8\nbranching=balanced\n", "[results]\noutcome=pending\n", {},
         {"Awaiting worker capacity"}},
        {41012, 2095, std::nullopt, "BattleSim", "SUPERSEDED", 1, "2026-03-20 09:22:17", "Superseded by rerun 41021", 2,
         {"09:22:17 CLAIMED by worker-1", "09:24:02 SUPERSEDED"},
         "[job]\nseed=10777\nmode=BattleTower\ntrainer=Palmer\n", "[results]\noutcome=superseded\n", {"old-summary.txt (2 KB)"},
         {"Superseded after new parameters were submitted"}},
        {41011, 2094, 884, "BattleSim", "SUCCEEDED_WINNER", 1, "2026-03-20 09:04:08", "Winner result promoted", 14,
         {"09:04:08 CLAIMED by worker-2", "09:05:01 RUNNING", "09:10:44 SUCCEEDED_WINNER"},
         "[job]\nseed=10601\nmode=BattleFactory\n", "[results]\noutcome=winner\nscore=55\n", {"winner.replay (900 KB)"},
         {"Winner promoted into summary set"}},
        {41010, 2094, 883, "BattleSim", "SUCCEEDED_DUPLICATE", 1, "2026-03-20 09:00:03", "Duplicate of winner job", 13,
         {"09:00:03 CLAIMED by worker-7", "09:00:44 RUNNING", "09:06:13 SUCCEEDED_DUPLICATE"},
         "[job]\nseed=10600\nmode=BattleFactory\n", "[results]\noutcome=duplicate\n", {"duplicate-note.txt (1 KB)"},
         {"Duplicate terminal state noted"}}
    };
}

void JobsPage::createWidgets()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(12);

    titleLabel_ = new QLabel("Jobs", this);
    titleLabel_->setObjectName("pageTitle");
    rootLayout->addWidget(titleLabel_);

    descriptionLabel_ = new QLabel(
        "Qt implementation of the Jobs workspace with mock filters, paging, selection, inspector interactions, and model/view tables.",
        this);
    descriptionLabel_->setObjectName("pageDescription");
    descriptionLabel_->setWordWrap(true);
    rootLayout->addWidget(descriptionLabel_);

    QFrame* filterPanel = new QFrame(this);
    filterPanel->setObjectName("jobsToolbarPanel");
    QGridLayout* filterLayout = new QGridLayout(filterPanel);
    filterLayout->setContentsMargins(16, 14, 16, 14);
    filterLayout->setHorizontalSpacing(12);
    filterLayout->setVerticalSpacing(10);

    kindFilter_ = new QComboBox(filterPanel);
    kindFilter_->setObjectName("jobsFilterCombo");
    stateFilter_ = new QComboBox(filterPanel);
    stateFilter_->setObjectName("jobsFilterCombo");
    jobSetFilter_ = new QLineEdit(filterPanel);
    jobSetFilter_->setObjectName("jobsFilterEdit");
    jobSetFilter_->setPlaceholderText("job_set_id");
    applyButton_ = new QPushButton("Apply", filterPanel);
    applyButton_->setObjectName("jobsPrimaryButton");
    resetButton_ = new QPushButton("Reset", filterPanel);
    resetButton_->setObjectName("jobsSecondaryButton");
    autoRefreshCheck_ = new QCheckBox("Auto refresh", filterPanel);
    autoRefreshCheck_->setObjectName("jobsCheckBox");
    refreshSecondsSpin_ = new QSpinBox(filterPanel);
    refreshSecondsSpin_->setObjectName("jobsRefreshSpin");
    refreshSecondsSpin_->setRange(1, 5);
    refreshSecondsSpin_->setSuffix(" s");

    kindFilter_->addItem("All kinds", QString());
    kindFilter_->addItems({"BattleSim", "Explorer", "SeedSearch"});

    stateFilter_->addItem("All states", QString());
    stateFilter_->addItems({
        "QUEUED", "CLAIMED", "RUNNING", "INTERRUPTED", "SUCCEEDED", "FAILED", "CANCELED",
        "SUPERSEDED", "SUCCEEDED_WINNER", "SUCCEEDED_DUPLICATE"
    });

    filterLayout->addWidget(new QLabel("Kind", filterPanel), 0, 0);
    filterLayout->addWidget(kindFilter_, 1, 0);
    filterLayout->addWidget(new QLabel("State", filterPanel), 0, 1);
    filterLayout->addWidget(stateFilter_, 1, 1);
    filterLayout->addWidget(new QLabel("Job Set", filterPanel), 0, 2);
    filterLayout->addWidget(jobSetFilter_, 1, 2);
    filterLayout->addWidget(applyButton_, 1, 3);
    filterLayout->addWidget(resetButton_, 1, 4);
    filterLayout->addWidget(autoRefreshCheck_, 0, 5, 1, 2, Qt::AlignBottom);
    filterLayout->addWidget(refreshSecondsSpin_, 1, 5);
    filterLayout->addWidget(new QLabel("Interval", filterPanel), 1, 6);
    filterLayout->setColumnStretch(2, 1);
    rootLayout->addWidget(filterPanel);

    QFrame* contentPanel = new QFrame(this);
    contentPanel->setObjectName("jobsContentPanel");
    QVBoxLayout* contentLayout = new QVBoxLayout(contentPanel);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(12);

    QFrame* pageControls = new QFrame(contentPanel);
    pageControls->setObjectName("jobsPagingPanel");
    QHBoxLayout* pageLayout = new QHBoxLayout(pageControls);
    pageLayout->setContentsMargins(16, 12, 16, 12);
    pageLayout->setSpacing(10);

    prevButton_ = new QPushButton("Prev", pageControls);
    prevButton_->setObjectName("jobsSecondaryButton");
    nextButton_ = new QPushButton("Next", pageControls);
    nextButton_->setObjectName("jobsSecondaryButton");
    refreshButton_ = new QPushButton("Refresh now", pageControls);
    refreshButton_->setObjectName("jobsSecondaryButton");
    pageSummaryLabel_ = new QLabel(pageControls);
    pageSummaryLabel_->setObjectName("jobsMetaText");
    lastRefreshLabel_ = new QLabel(pageControls);
    lastRefreshLabel_->setObjectName("jobsMetaText");

    pageLayout->addWidget(prevButton_);
    pageLayout->addWidget(nextButton_);
    pageLayout->addWidget(refreshButton_);
    pageLayout->addSpacing(8);
    pageLayout->addWidget(pageSummaryLabel_);
    pageLayout->addStretch();
    pageLayout->addWidget(lastRefreshLabel_);
    contentLayout->addWidget(pageControls);

    QSplitter* splitter = new QSplitter(Qt::Horizontal, contentPanel);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(8);

    QFrame* tablePanel = new QFrame(splitter);
    tablePanel->setObjectName("jobsSurfacePanel");
    QVBoxLayout* tableLayout = new QVBoxLayout(tablePanel);
    tableLayout->setContentsMargins(16, 16, 16, 16);
    tableLayout->setSpacing(10);

    QLabel* tableTitle = new QLabel("Jobs Table", tablePanel);
    tableTitle->setObjectName("panelTitle");
    QLabel* tableBody = new QLabel(
        "Single-click updates the inspector. Double-click also focuses the Overview tab, now using Qt's model/view table architecture.",
        tablePanel);
    tableBody->setObjectName("panelBody");
    tableBody->setWordWrap(true);

    jobsTable_ = new JobsTableView(tablePanel);
    jobsModel_ = new JobsTableModel(jobsTable_);
    jobsTable_->attachModel(jobsModel_);

    tableLayout->addWidget(tableTitle);
    tableLayout->addWidget(tableBody);
    tableLayout->addWidget(jobsTable_, 1);

    QFrame* inspectorPanel = new QFrame(splitter);
    inspectorPanel->setObjectName("jobsSurfacePanel");
    QVBoxLayout* inspectorLayout = new QVBoxLayout(inspectorPanel);
    inspectorLayout->setContentsMargins(16, 16, 16, 16);
    inspectorLayout->setSpacing(10);

    QLabel* inspectorTitle = new QLabel("Job Inspector", inspectorPanel);
    inspectorTitle->setObjectName("panelTitle");
    inspectorSummary_ = new QLabel("Select a job to inspect details.", inspectorPanel);
    inspectorSummary_->setObjectName("jobsInspectorSummary");
    inspectorSummary_->setWordWrap(true);

    QHBoxLayout* actionLayout = new QHBoxLayout();
    actionLayout->setSpacing(8);
    requeueButton_ = new QPushButton("Requeue", inspectorPanel);
    requeueButton_->setObjectName("jobsSecondaryButton");
    restartButton_ = new QPushButton("Restart", inspectorPanel);
    restartButton_->setObjectName("jobsSecondaryButton");
    cancelButton_ = new QPushButton("Cancel", inspectorPanel);
    cancelButton_->setObjectName("jobsSecondaryButton");
    bumpDeltaSpin_ = new QSpinBox(inspectorPanel);
    bumpDeltaSpin_->setObjectName("jobsRefreshSpin");
    bumpDeltaSpin_->setRange(-9, 9);
    bumpDeltaSpin_->setValue(1);
    applyBumpButton_ = new QPushButton("Apply bump", inspectorPanel);
    applyBumpButton_->setObjectName("jobsPrimaryButton");
    inspectorRefreshButton_ = new QPushButton("Refresh detail", inspectorPanel);
    inspectorRefreshButton_->setObjectName("jobsSecondaryButton");
    actionLayout->addWidget(requeueButton_);
    actionLayout->addWidget(restartButton_);
    actionLayout->addWidget(cancelButton_);
    actionLayout->addWidget(new QLabel("Delta", inspectorPanel));
    actionLayout->addWidget(bumpDeltaSpin_);
    actionLayout->addWidget(applyBumpButton_);
    actionLayout->addStretch();
    actionLayout->addWidget(inspectorRefreshButton_);

    inspectorTabs_ = new QTabWidget(inspectorPanel);
    inspectorTabs_->setObjectName("jobsInspectorTabs");

    QWidget* overviewTab = new QWidget(inspectorTabs_);
    QVBoxLayout* overviewLayout = new QVBoxLayout(overviewTab);
    overviewLayout->setContentsMargins(12, 12, 12, 12);
    overviewLayout->setSpacing(10);
    QGridLayout* overviewGrid = new QGridLayout();
    overviewGrid->setHorizontalSpacing(10);
    overviewGrid->setVerticalSpacing(8);
    overviewPriorityValue_ = new QLabel("--", overviewTab);
    overviewPriorityValue_->setObjectName("jobsValueLabel");
    overviewQueuedValue_ = new QLabel("--", overviewTab);
    overviewQueuedValue_->setObjectName("jobsValueLabel");
    overviewSelectionHint_ = new QLabel("Choose a row from the table to populate the inspector.", overviewTab);
    overviewSelectionHint_->setObjectName("panelBody");
    overviewSelectionHint_->setWordWrap(true);
    overviewGrid->addWidget(new QLabel("Priority", overviewTab), 0, 0);
    overviewGrid->addWidget(overviewPriorityValue_, 0, 1);
    overviewGrid->addWidget(new QLabel("Queued At", overviewTab), 1, 0);
    overviewGrid->addWidget(overviewQueuedValue_, 1, 1);
    overviewLayout->addLayout(overviewGrid);
    overviewLayout->addWidget(overviewSelectionHint_);
    overviewLayout->addStretch();

    eventsText_ = createReadOnlyTextEdit();
    progressText_ = createReadOnlyTextEdit();
    payloadText_ = createReadOnlyTextEdit();
    resultsText_ = createReadOnlyTextEdit();

    QWidget* artifactsTab = new QWidget(inspectorTabs_);
    QVBoxLayout* artifactsLayout = new QVBoxLayout(artifactsTab);
    artifactsLayout->setContentsMargins(12, 12, 12, 12);
    artifactsLayout->setSpacing(8);
    artifactsTable_ = new ArtifactsTableView(artifactsTab);
    artifactsModel_ = new ArtifactsTableModel(artifactsTable_);
    artifactsTable_->attachModel(artifactsModel_);
    artifactsLayout->addWidget(artifactsTable_);

    inspectorTabs_->addTab(overviewTab, "Overview");
    inspectorTabs_->addTab(eventsText_, "Events");
    inspectorTabs_->addTab(payloadText_, "Payload");
    inspectorTabs_->addTab(artifactsTab, "Artifacts");
    inspectorTabs_->addTab(progressText_, "Progress");
    inspectorTabs_->addTab(resultsText_, "Results");

    inspectorLayout->addWidget(inspectorTitle);
    inspectorLayout->addWidget(inspectorSummary_);
    inspectorLayout->addLayout(actionLayout);
    inspectorLayout->addWidget(inspectorTabs_, 1);

    splitter->addWidget(tablePanel);
    splitter->addWidget(inspectorPanel);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({900, 520});

    contentLayout->addWidget(splitter, 1);
    rootLayout->addWidget(contentPanel, 1);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->start(refreshSeconds_ * 1000);
}

QTextEdit* JobsPage::createReadOnlyTextEdit()
{
    QTextEdit* edit = new QTextEdit(this);
    edit->setObjectName("jobsInspectorText");
    edit->setReadOnly(true);
    return edit;
}

void JobsPage::wireSignals()
{
    connect(applyButton_, &QPushButton::clicked, this, [this]() {
        selectedProgramKind_ = kindFilter_->currentText() == "All kinds" ? QString() : kindFilter_->currentText();
        selectedState_ = stateFilter_->currentText() == "All states" ? QString() : stateFilter_->currentText();
        selectedJobSetId_ = jobSetFilter_->text().trimmed();
        autoRefreshEnabled_ = autoRefreshCheck_->isChecked();
        refreshSeconds_ = refreshSecondsSpin_->value();
        refreshTimer_->setInterval(refreshSeconds_ * 1000);
        pageStartIndex_ = 0;
        applyFilters();
        refreshMockProgress();
    });

    connect(resetButton_, &QPushButton::clicked, this, [this]() {
        selectedProgramKind_.clear();
        selectedState_.clear();
        selectedJobSetId_.clear();
        autoRefreshEnabled_ = true;
        refreshSeconds_ = 2;
        pageStartIndex_ = 0;
        syncControlsToState();
        applyFilters();
        refreshMockProgress();
    });

    connect(autoRefreshCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        autoRefreshEnabled_ = checked;
    });

    connect(refreshSecondsSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        refreshSeconds_ = value;
        refreshTimer_->setInterval(refreshSeconds_ * 1000);
    });

    connect(prevButton_, &QPushButton::clicked, this, [this]() {
        pageStartIndex_ = std::max(0, pageStartIndex_ - kJobsPageSize);
        populateTable();
    });

    connect(nextButton_, &QPushButton::clicked, this, [this]() {
        if (pageStartIndex_ + kJobsPageSize < static_cast<int>(filteredJobs_.size())) {
            pageStartIndex_ += kJobsPageSize;
            populateTable();
        }
    });

    connect(refreshButton_, &QPushButton::clicked, this, [this]() {
        refreshMockProgress();
    });

    connect(refreshTimer_, &QTimer::timeout, this, [this]() {
        if (autoRefreshEnabled_) {
            refreshMockProgress();
        }
    });

    connect(jobsTable_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            if (!current.isValid()) {
                return;
            }
            if (const JobsTableModel::Row* row = jobsModel_->rowAt(current.row())) {
                selectedJobId_ = row->jobId;
                if (const MockJob* job = selectedJob()) {
                    updateInspector(*job);
                }
            }
        });

    connect(jobsTable_, &JobsTableView::doubleClicked, this, [this](const QModelIndex& current) {
        if (current.isValid()) {
            jobsTable_->selectRow(current.row());
            inspectorTabs_->setCurrentIndex(0);
            inspectorTabs_->setFocus();
        }
    });

    const auto triggerSelectionRefresh = [this]() {
        if (const MockJob* job = selectedJob()) {
            updateInspector(*job);
        }
    };

    connect(requeueButton_, &QPushButton::clicked, this, [this, triggerSelectionRefresh]() {
        if (MockJob* job = selectedJobMutable()) {
            job->state = "QUEUED";
            job->progress = "Requeued from inspector";
            job->events.prepend(timestampPrefix() + " REQUEUED manually");
            triggerSelectionRefresh();
            populateTable();
        }
    });

    connect(restartButton_, &QPushButton::clicked, this, [this, triggerSelectionRefresh]() {
        if (MockJob* job = selectedJobMutable()) {
            job->state = "QUEUED";
            job->attempts = 0;
            job->progress = "Restart requested from inspector";
            job->events.prepend(timestampPrefix() + " RESTART requested");
            triggerSelectionRefresh();
            populateTable();
        }
    });

    connect(cancelButton_, &QPushButton::clicked, this, [this, triggerSelectionRefresh]() {
        if (MockJob* job = selectedJobMutable()) {
            job->state = "CANCELED";
            job->progress = "Canceled from inspector";
            job->events.prepend(timestampPrefix() + " CANCELED manually");
            triggerSelectionRefresh();
            populateTable();
        }
    });

    connect(applyBumpButton_, &QPushButton::clicked, this, [this, triggerSelectionRefresh]() {
        if (MockJob* job = selectedJobMutable()) {
            job->priority += bumpDeltaSpin_->value();
            job->events.prepend(timestampPrefix() + QString(" PRIORITY bumped by %1").arg(bumpDeltaSpin_->value()));
            triggerSelectionRefresh();
        }
    });

    connect(inspectorRefreshButton_, &QPushButton::clicked, this, [this, triggerSelectionRefresh]() {
        if (MockJob* job = selectedJobMutable()) {
            job->events.prepend(timestampPrefix() + " DETAIL refresh requested");
            triggerSelectionRefresh();
        }
    });
}

void JobsPage::syncControlsToState()
{
    {
        QSignalBlocker blocker(kindFilter_);
        kindFilter_->setCurrentIndex(selectedProgramKind_.isEmpty() ? 0 : std::max(0, kindFilter_->findText(selectedProgramKind_)));
    }
    {
        QSignalBlocker blocker(stateFilter_);
        stateFilter_->setCurrentIndex(selectedState_.isEmpty() ? 0 : std::max(0, stateFilter_->findText(selectedState_)));
    }
    jobSetFilter_->setText(selectedJobSetId_);
    autoRefreshCheck_->setChecked(autoRefreshEnabled_);
    refreshSecondsSpin_->setValue(refreshSeconds_);
}

void JobsPage::applyFilters()
{
    filteredJobs_.clear();
    for (const MockJob& job : allJobs_) {
        if (!selectedProgramKind_.isEmpty() && job.programKind != selectedProgramKind_) {
            continue;
        }
        if (!selectedState_.isEmpty() && job.state != selectedState_) {
            continue;
        }
        if (!selectedJobSetId_.isEmpty() && QString::number(job.jobSetId) != selectedJobSetId_) {
            continue;
        }
        filteredJobs_.push_back(&job);
    }

    if (pageStartIndex_ >= static_cast<int>(filteredJobs_.size())) {
        pageStartIndex_ = std::max(0, static_cast<int>(filteredJobs_.size()) - kJobsPageSize);
    }

    populateTable();
    syncInspectorAfterFilter();
    updatePageControls();
}

void JobsPage::populateTable()
{
    const int endIndex = std::min(pageStartIndex_ + kJobsPageSize, static_cast<int>(filteredJobs_.size()));
    std::vector<JobsTableModel::Row> visibleRows;
    visibleRows.reserve(std::max(0, endIndex - pageStartIndex_));
    for (int i = pageStartIndex_; i < endIndex; ++i) {
        const MockJob& job = *filteredJobs_[i];
        visibleRows.push_back(JobsTableModel::Row{ job.jobId, job.jobSetId, job.savestateId, job.programKind, job.state, job.attempts, job.queuedAt, job.progress });
    }

    jobsModel_->setRows(visibleRows);

    if (const MockJob* selected = selectedJob()) {
        const int visibleRow = visibleRowForJob(selected->jobId);
        if (visibleRow >= 0) {
            jobsTable_->selectRow(visibleRow);
            jobsTable_->scrollTo(jobsModel_->index(visibleRow, 0));
        }
    }

    updatePageControls();
}

void JobsPage::updatePageControls()
{
    const int total = static_cast<int>(filteredJobs_.size());
    const int start = total == 0 ? 0 : pageStartIndex_ + 1;
    const int end = std::min(pageStartIndex_ + kJobsPageSize, total);
    pageSummaryLabel_->setText(QString("Showing %1-%2 of %3 mock jobs").arg(start).arg(end).arg(total));
    prevButton_->setEnabled(pageStartIndex_ > 0);
    nextButton_->setEnabled(pageStartIndex_ + kJobsPageSize < total);
    lastRefreshLabel_->setText(QString("Last refresh: %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss")));
}

void JobsPage::syncInspectorAfterFilter()
{
    if (const MockJob* current = selectedJob()) {
        updateInspector(*current);
        return;
    }

    if (!filteredJobs_.empty()) {
        selectedJobId_ = filteredJobs_.front()->jobId;
        updateInspector(*filteredJobs_.front());
    } else {
        selectedJobId_ = 0;
        clearInspector();
    }
}

void JobsPage::updateInspector(const MockJob& job)
{
    inspectorSummary_->setText(
        QString("Job %1 | Set %2 | ProgramKind %3 | State %4")
            .arg(job.jobId)
            .arg(job.jobSetId)
            .arg(job.programKind)
            .arg(job.state));
    overviewPriorityValue_->setText(QString::number(job.priority));
    overviewQueuedValue_->setText(job.queuedAt);
    overviewSelectionHint_->setText(QString("Attempts: %1\nProgress: %2").arg(job.attempts).arg(job.progress));

    eventsText_->setPlainText(job.events.join('\n'));
    progressText_->setPlainText(job.decodedProgress.join('\n'));
    payloadText_->setPlainText(job.payload);
    resultsText_->setPlainText(job.results);
    artifactsModel_->setArtifacts(job.artifacts);
    artifactsTable_->resizeColumnsToContents();

    requeueButton_->setEnabled(job.state != "RUNNING" && job.state != "CLAIMED");
    restartButton_->setEnabled(job.state == "FAILED");
    cancelButton_->setEnabled(job.state != "SUCCEEDED" && job.state != "CANCELED" && job.state != "SUCCEEDED_WINNER" && job.state != "SUCCEEDED_DUPLICATE");
    applyBumpButton_->setEnabled(true);
    inspectorRefreshButton_->setEnabled(true);
}

void JobsPage::clearInspector()
{
    inspectorSummary_->setText("Select a job to inspect details.");
    overviewPriorityValue_->setText("--");
    overviewQueuedValue_->setText("--");
    overviewSelectionHint_->setText("No jobs match the current filters.");
    eventsText_->clear();
    progressText_->clear();
    payloadText_->clear();
    resultsText_->clear();
    artifactsModel_->setArtifacts({});
    requeueButton_->setEnabled(false);
    restartButton_->setEnabled(false);
    cancelButton_->setEnabled(false);
    applyBumpButton_->setEnabled(false);
    inspectorRefreshButton_->setEnabled(false);
}

int JobsPage::visibleRowForJob(qint64 jobId) const
{
    const int endIndex = std::min(pageStartIndex_ + kJobsPageSize, static_cast<int>(filteredJobs_.size()));
    for (int i = pageStartIndex_; i < endIndex; ++i) {
        if (filteredJobs_[i]->jobId == jobId) {
            return i - pageStartIndex_;
        }
    }
    return -1;
}

const JobsPage::MockJob* JobsPage::selectedJob() const
{
    auto it = std::find_if(allJobs_.cbegin(), allJobs_.cend(), [this](const MockJob& job) {
        return job.jobId == selectedJobId_;
    });
    return it == allJobs_.cend() ? nullptr : &(*it);
}

JobsPage::MockJob* JobsPage::selectedJobMutable()
{
    auto it = std::find_if(allJobs_.begin(), allJobs_.end(), [this](const MockJob& job) {
        return job.jobId == selectedJobId_;
    });
    return it == allJobs_.end() ? nullptr : &(*it);
}

void JobsPage::refreshMockProgress()
{
    for (MockJob& job : allJobs_) {
        if (job.state == "RUNNING") {
            job.progress += " • refreshed";
            job.events.prepend(timestampPrefix() + " AUTO refresh snapshot");
        } else if (job.state == "QUEUED") {
            job.progress = "Queued • refreshed at " + QDateTime::currentDateTime().toString("hh:mm:ss");
        }
    }
    populateTable();
    if (const MockJob* current = selectedJob()) {
        updateInspector(*current);
    }
    updatePageControls();
}

QString JobsPage::timestampPrefix() const
{
    return QDateTime::currentDateTime().toString("hh:mm:ss");
}
