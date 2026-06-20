#include "ArchiveWorkbenchPage.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDateTime>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimeZone>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDateTimeEdit>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <limits>
#include <sstream>

namespace {
constexpr int kWorkflowIdRole = Qt::UserRole + 1;

QString qstr(const std::string& value)
{
    return QString::fromStdString(value);
}

QString formatOptionalTime(const std::optional<std::int64_t>& epochMillis)
{
    if (!epochMillis.has_value() || *epochMillis <= 0) {
        return QStringLiteral("-");
    }
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(*epochMillis), QTimeZone::fromSecondsAheadOfUtc(0))
        .toLocalTime()
        .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
}

QString formatTime(std::int64_t epochMillis)
{
    return formatOptionalTime(epochMillis > 0 ? std::optional<std::int64_t>(epochMillis) : std::nullopt);
}

QTableWidgetItem* makeItem(const QString& text)
{
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QString joinMessages(const std::vector<std::string>& values)
{
    QStringList out;
    for (const auto& value : values) {
        if (!value.empty()) {
            out.push_back(qstr(value));
        }
    }
    return out.join(QStringLiteral(" | "));
}

std::vector<std::int64_t> toVector(const std::set<std::int64_t>& values)
{
    return std::vector<std::int64_t>(values.begin(), values.end());
}

QString progressPhaseText(savor::db::archive::ArchiveOperationPhase phase)
{
    using Phase = savor::db::archive::ArchiveOperationPhase;
    switch (phase) {
    case Phase::Previewing: return QStringLiteral("Previewing");
    case Phase::ExportingRows: return QStringLiteral("Exporting rows");
    case Phase::WritingSavestates: return QStringLiteral("Writing savestates");
    case Phase::RegisteringPackage: return QStringLiteral("Registering package");
    case Phase::VerifyingPackage: return QStringLiteral("Verifying package");
    case Phase::PurgingSource: return QStringLiteral("Purging source");
    case Phase::Complete: return QStringLiteral("Complete");
    case Phase::Failed: return QStringLiteral("Failed");
    }
    return QStringLiteral("Archive");
}
}

ArchiveWorkbenchPage::ArchiveWorkbenchPage(QWidget* parent)
    : QWidget(parent)
{
    createWidgets();
    wireSignals();
}

void ArchiveWorkbenchPage::setPageActive(bool active)
{
    pageActive_ = active;
    if (candidateRefreshPipeline_ != nullptr) {
        candidateRefreshPipeline_->setActive(active);
    }
    if (previewRefreshPipeline_ != nullptr) {
        previewRefreshPipeline_->setActive(active);
    }
    if (active) {
        refreshCandidates();
    }
}

void ArchiveWorkbenchPage::createWidgets()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    auto* filterPanel = new QFrame(this);
    filterPanel->setObjectName("jobSetsToolbarPanel");
    auto* filterLayout = new QGridLayout(filterPanel);
    filterLayout->setContentsMargins(8, 7, 8, 7);
    filterLayout->setHorizontalSpacing(10);
    filterLayout->setVerticalSpacing(8);

    displayStateFilter_ = new QComboBox(filterPanel);
    displayStateFilter_->setObjectName("jobSetsFilterCombo");
    displayStateFilter_->addItem(QStringLiteral("All states"), QString());
    displayStateFilter_->addItem(QStringLiteral("Completed"), QStringLiteral("COMPLETED"));
    displayStateFilter_->addItem(QStringLiteral("Failed"), QStringLiteral("FAILED"));
    displayStateFilter_->addItem(QStringLiteral("Canceled"), QStringLiteral("CANCELED"));
    displayStateFilter_->addItem(QStringLiteral("Running"), QStringLiteral("RUNNING"));

    workflowKindFilter_ = new QLineEdit(filterPanel);
    workflowKindFilter_->setObjectName("jobSetsFilterCombo");
    workflowKindFilter_->setPlaceholderText(QStringLiteral("workflow kind"));

    finalVictoryFilter_ = new QComboBox(filterPanel);
    finalVictoryFilter_->setObjectName("jobSetsFilterCombo");
    finalVictoryFilter_->addItem(QStringLiteral("Any victory"), static_cast<int>(savorqt::db::ArchiveFinalVictoryMode::Any));
    finalVictoryFilter_->addItem(QStringLiteral("Victory present"), static_cast<int>(savorqt::db::ArchiveFinalVictoryMode::Present));
    finalVictoryFilter_->addItem(QStringLiteral("Victory absent"), static_cast<int>(savorqt::db::ArchiveFinalVictoryMode::Absent));

    problemFilter_ = new QComboBox(filterPanel);
    problemFilter_->setObjectName("jobSetsFilterCombo");
    problemFilter_->addItem(QStringLiteral("Any problems"), static_cast<int>(savorqt::db::ArchiveProblemMode::Any));
    problemFilter_->addItem(QStringLiteral("Has problems"), static_cast<int>(savorqt::db::ArchiveProblemMode::HasProblems));
    problemFilter_->addItem(QStringLiteral("No problems"), static_cast<int>(savorqt::db::ArchiveProblemMode::NoProblems));

    textFilter_ = new QLineEdit(filterPanel);
    textFilter_->setObjectName("jobSetsFilterCombo");
    textFilter_->setPlaceholderText(QStringLiteral("id/kind/state"));

    const auto now = QDateTime::currentDateTime();
    createdFromEnabled_ = new QCheckBox(QStringLiteral("Created from"), filterPanel);
    createdToEnabled_ = new QCheckBox(QStringLiteral("Created to"), filterPanel);
    completedFromEnabled_ = new QCheckBox(QStringLiteral("Completed from"), filterPanel);
    completedToEnabled_ = new QCheckBox(QStringLiteral("Completed to"), filterPanel);
    createdFromEdit_ = new QDateTimeEdit(now.addDays(-7), filterPanel);
    createdToEdit_ = new QDateTimeEdit(now, filterPanel);
    completedFromEdit_ = new QDateTimeEdit(now.addDays(-7), filterPanel);
    completedToEdit_ = new QDateTimeEdit(now, filterPanel);
    for (auto* edit : { createdFromEdit_, createdToEdit_, completedFromEdit_, completedToEdit_ }) {
        edit->setObjectName("jobSetsFilterCombo");
        edit->setCalendarPopup(true);
        edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
    }

    applyButton_ = new QPushButton(QStringLiteral("Apply"), filterPanel);
    applyButton_->setObjectName("jobSetsPrimaryButton");
    refreshButton_ = new QPushButton(QStringLiteral("Refresh"), filterPanel);
    refreshButton_->setObjectName("jobSetsSecondaryButton");

    filterLayout->addWidget(new QLabel(QStringLiteral("State"), filterPanel), 0, 0);
    filterLayout->addWidget(displayStateFilter_, 1, 0);
    filterLayout->addWidget(new QLabel(QStringLiteral("Kind"), filterPanel), 0, 1);
    filterLayout->addWidget(workflowKindFilter_, 1, 1);
    filterLayout->addWidget(new QLabel(QStringLiteral("Victory"), filterPanel), 0, 2);
    filterLayout->addWidget(finalVictoryFilter_, 1, 2);
    filterLayout->addWidget(new QLabel(QStringLiteral("Problems"), filterPanel), 0, 3);
    filterLayout->addWidget(problemFilter_, 1, 3);
    filterLayout->addWidget(new QLabel(QStringLiteral("Search"), filterPanel), 0, 4);
    filterLayout->addWidget(textFilter_, 1, 4);
    filterLayout->addWidget(applyButton_, 1, 5);
    filterLayout->addWidget(refreshButton_, 1, 6);
    filterLayout->addWidget(createdFromEnabled_, 2, 0);
    filterLayout->addWidget(createdFromEdit_, 3, 0);
    filterLayout->addWidget(createdToEnabled_, 2, 1);
    filterLayout->addWidget(createdToEdit_, 3, 1);
    filterLayout->addWidget(completedFromEnabled_, 2, 2);
    filterLayout->addWidget(completedFromEdit_, 3, 2);
    filterLayout->addWidget(completedToEnabled_, 2, 3);
    filterLayout->addWidget(completedToEdit_, 3, 3);
    filterLayout->setColumnStretch(4, 1);
    root->addWidget(filterPanel);

    auto* actionsPanel = new QFrame(this);
    actionsPanel->setObjectName("jobSetsPagingPanel");
    auto* actionsLayout = new QHBoxLayout(actionsPanel);
    actionsLayout->setContentsMargins(8, 7, 8, 7);
    actionsLayout->setSpacing(8);
    selectVisibleButton_ = new QPushButton(QStringLiteral("Select all shown"), actionsPanel);
    clearVisibleButton_ = new QPushButton(QStringLiteral("Clear all shown"), actionsPanel);
    selectAllMatchingButton_ = new QPushButton(QStringLiteral("Select all matching"), actionsPanel);
    excludeSelectedButton_ = new QPushButton(QStringLiteral("Exclude highlighted"), actionsPanel);
    clearExclusionsButton_ = new QPushButton(QStringLiteral("Clear exclusions"), actionsPanel);
    statusLabel_ = new QLabel(actionsPanel);
    statusLabel_->setObjectName("jobSetsMetaText");
    actionsLayout->addWidget(selectVisibleButton_);
    actionsLayout->addWidget(clearVisibleButton_);
    actionsLayout->addWidget(selectAllMatchingButton_);
    actionsLayout->addWidget(excludeSelectedButton_);
    actionsLayout->addWidget(clearExclusionsButton_);
    actionsLayout->addStretch();
    actionsLayout->addWidget(statusLabel_);
    root->addWidget(actionsPanel);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    candidateTable_ = new QTableWidget(splitter);
    candidateTable_->setObjectName("jobSetsTreeView");
    candidateTable_->setColumnCount(10);
    candidateTable_->setHorizontalHeaderLabels({
        QStringLiteral("Include"),
        QStringLiteral("Workflow"),
        QStringLiteral("Kind"),
        QStringLiteral("State"),
        QStringLiteral("Created"),
        QStringLiteral("Completed"),
        QStringLiteral("Failed"),
        QStringLiteral("Blocked"),
        QStringLiteral("Victories"),
        QStringLiteral("Selection"),
    });
    candidateTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    candidateTable_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    candidateTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    candidateTable_->setAlternatingRowColors(true);
    candidateTable_->verticalHeader()->hide();
    candidateTable_->horizontalHeader()->setStretchLastSection(false);
    candidateTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    candidateTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    candidateTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    candidateTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    candidateTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Interactive);
    candidateTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Interactive);
    candidateTable_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Interactive);
    candidateTable_->horizontalHeader()->setSectionResizeMode(7, QHeaderView::Interactive);
    candidateTable_->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Interactive);
    candidateTable_->horizontalHeader()->setSectionResizeMode(9, QHeaderView::Stretch);
    splitter->addWidget(candidateTable_);

    auto* sidePanel = new QWidget(splitter);
    auto* sideLayout = new QVBoxLayout(sidePanel);
    sideLayout->setContentsMargins(8, 0, 0, 0);
    previewLabel_ = new QLabel(sidePanel);
    previewLabel_->setObjectName("jobSetsMetaText");
    previewLabel_->setWordWrap(true);
    blockersLabel_ = new QLabel(sidePanel);
    blockersLabel_->setObjectName("jobSetsInlineMessage");
    blockersLabel_->setWordWrap(true);
    packageOnlyCheck_ = new QCheckBox(QStringLiteral("Package only"), sidePanel);
    packageOnlyCheck_->setChecked(true);
    purgeAfterVerifyCheck_ = new QCheckBox(QStringLiteral("Purge after verify"), sidePanel);
    archiveNameEdit_ = new QLineEdit(sidePanel);
    archiveNameEdit_->setPlaceholderText(QStringLiteral("generated if blank"));
    archiveNotesEdit_ = new QTextEdit(sidePanel);
    archiveNotesEdit_->setPlaceholderText(QStringLiteral("optional notes"));
    archiveNotesEdit_->setAcceptRichText(false);
    archiveNotesEdit_->setFixedHeight(72);
    confirmationEdit_ = new QLineEdit(sidePanel);
    confirmationEdit_->setPlaceholderText(QStringLiteral("selected workflow count"));
    executeButton_ = new QPushButton(QStringLiteral("Create archive"), sidePanel);
    executeButton_->setObjectName("jobSetsPrimaryButton");
    archiveProgressPhaseLabel_ = new QLabel(QStringLiteral("Idle"), sidePanel);
    archiveProgressPhaseLabel_->setObjectName("jobSetsMetaText");
    archiveProgressPhaseLabel_->setWordWrap(true);
    archiveProgressBar_ = new QProgressBar(sidePanel);
    archiveProgressBar_->setRange(0, 1);
    archiveProgressBar_->setValue(0);
    archiveProgressBar_->setTextVisible(true);
    executeStatusLabel_ = new QLabel(sidePanel);
    executeStatusLabel_->setObjectName("jobSetsMetaText");
    executeStatusLabel_->setWordWrap(true);
    sideLayout->addWidget(new QLabel(QStringLiteral("Preview"), sidePanel));
    sideLayout->addWidget(previewLabel_);
    sideLayout->addWidget(blockersLabel_);
    sideLayout->addSpacing(12);
    sideLayout->addWidget(new QLabel(QStringLiteral("Execute"), sidePanel));
    sideLayout->addWidget(packageOnlyCheck_);
    sideLayout->addWidget(purgeAfterVerifyCheck_);
    sideLayout->addWidget(new QLabel(QStringLiteral("Archive name"), sidePanel));
    sideLayout->addWidget(archiveNameEdit_);
    sideLayout->addWidget(new QLabel(QStringLiteral("Notes"), sidePanel));
    sideLayout->addWidget(archiveNotesEdit_);
    sideLayout->addWidget(confirmationEdit_);
    sideLayout->addWidget(executeButton_);
    sideLayout->addWidget(archiveProgressPhaseLabel_);
    sideLayout->addWidget(archiveProgressBar_);
    sideLayout->addWidget(executeStatusLabel_);
    sideLayout->addStretch();
    splitter->addWidget(sidePanel);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 1);
    root->addWidget(splitter, 1);

    candidateRefreshPipeline_ = new savorqt::gui::AsyncRefreshPipeline<CandidateRefreshRequest, CandidateResult>(this);
    previewRefreshPipeline_ = new savorqt::gui::AsyncRefreshPipeline<PreviewRefreshRequest, PreviewResult>(this);
}

void ArchiveWorkbenchPage::wireSignals()
{
    connect(applyButton_, &QPushButton::clicked, this, [this]() {
        refreshCandidates();
    });
    connect(refreshButton_, &QPushButton::clicked, this, &ArchiveWorkbenchPage::refreshCandidates);
    connect(selectVisibleButton_, &QPushButton::clicked, this, [this]() { setVisibleRowsSelected(true); });
    connect(clearVisibleButton_, &QPushButton::clicked, this, [this]() { setVisibleRowsSelected(false); });
    connect(selectAllMatchingButton_, &QPushButton::clicked, this, [this]() {
        includeFilterMatches_ = true;
        requestPreview();
        refreshCandidates();
    });
    connect(excludeSelectedButton_, &QPushButton::clicked, this, &ArchiveWorkbenchPage::markHighlightedRowsExcluded);
    connect(clearExclusionsButton_, &QPushButton::clicked, this, [this]() {
        explicitExclusions_.clear();
        requestPreview();
        refreshCandidates();
    });
    connect(packageOnlyCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && purgeAfterVerifyCheck_->isChecked()) {
            QSignalBlocker blocker(purgeAfterVerifyCheck_);
            purgeAfterVerifyCheck_->setChecked(false);
        } else if (!checked && !purgeAfterVerifyCheck_->isChecked()) {
            QSignalBlocker blocker(packageOnlyCheck_);
            packageOnlyCheck_->setChecked(true);
        }
        updateExecuteState();
    });
    connect(purgeAfterVerifyCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && packageOnlyCheck_->isChecked()) {
            QSignalBlocker blocker(packageOnlyCheck_);
            packageOnlyCheck_->setChecked(false);
        } else if (!checked && !packageOnlyCheck_->isChecked()) {
            QSignalBlocker blocker(packageOnlyCheck_);
            packageOnlyCheck_->setChecked(true);
        }
        updateExecuteState();
    });
    connect(confirmationEdit_, &QLineEdit::textChanged, this, &ArchiveWorkbenchPage::updateExecuteState);
    connect(executeButton_, &QPushButton::clicked, this, &ArchiveWorkbenchPage::executeArchive);
    connect(candidateTable_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (item == nullptr || item->column() != 0) return;
        const auto id = item->data(kWorkflowIdRole).toLongLong();
        if (id <= 0) return;
        if (item->checkState() == Qt::Checked) {
            explicitExclusions_.erase(id);
            explicitIncludes_.insert(id);
        } else {
            explicitIncludes_.erase(id);
            if (includeFilterMatches_) {
                explicitExclusions_.insert(id);
            }
        }
        requestPreview();
        refreshCandidates();
    });

    candidateRefreshPipeline_->setAutoRefreshEnabled(false);
    candidateRefreshPipeline_->setRequestBuilder([this](savorqt::gui::RefreshReason) -> std::optional<CandidateRefreshRequest> {
        candidateFetchInFlight_ = true;
        updateInlineMessage();
        return CandidateRefreshRequest{ .filter = currentFilter() };
    });
    candidateRefreshPipeline_->setLoadAndPrepare([](CandidateRefreshRequest request) {
        const auto page = savorqt::db::SavorDbArchiveService::ListWorkflowCandidates(request.filter);
        if (!page.ok) {
            return savorqt::gui::AsyncRefreshResult<CandidateResult>::Ok(CandidateResult::Err(page.error));
        }
        CandidateRefreshData data{};
        data.filter = request.filter;
        data.page = page.value;
        return savorqt::gui::AsyncRefreshResult<CandidateResult>::Ok(CandidateResult::Ok(std::move(data)));
    });
    candidateRefreshPipeline_->setApply([this](const CandidateResult& result, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        candidateFetchInFlight_ = false;
        if (!result.ok) {
            inlineMessage_ = QStringLiteral("Candidate refresh failed: %1").arg(qstr(result.error.message));
            applyCandidateRows({});
        } else {
            inlineMessage_.clear();
            applyCandidateRows(result.value.page.rows);
        }
        updateInlineMessage();
        requestPreview();
    });

    previewRefreshPipeline_->setAutoRefreshEnabled(false);
    previewRefreshPipeline_->setRequestBuilder([this](savorqt::gui::RefreshReason) -> std::optional<PreviewRefreshRequest> {
        previewFetchInFlight_ = true;
        updatePreviewLabels();
        return PreviewRefreshRequest{ .selection_request = currentSelectionRequest() };
    });
    previewRefreshPipeline_->setLoadAndPrepare([](PreviewRefreshRequest request) {
        const auto selection = savorqt::db::SavorDbArchiveService::BuildSelection(request.selection_request);
        if (!selection.ok) {
            return savorqt::gui::AsyncRefreshResult<PreviewResult>::Ok(PreviewResult::Err(selection.error));
        }
        PreviewRefreshData data{};
        data.selection = selection.value;
        if (data.selection.selected_count > 0) {
            const auto preview = savorqt::db::SavorDbArchiveService::PreviewWorkflowArchive(data.selection.selection);
            if (!preview.ok) {
                return savorqt::gui::AsyncRefreshResult<PreviewResult>::Ok(PreviewResult::Err(preview.error));
            }
            data.preview = preview.value;
        }
        return savorqt::gui::AsyncRefreshResult<PreviewResult>::Ok(PreviewResult::Ok(std::move(data)));
    });
    previewRefreshPipeline_->setApply([this](const PreviewResult& result, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        previewFetchInFlight_ = false;
        if (!result.ok) {
            currentPreview_.reset();
            inlineMessage_ = QStringLiteral("Preview failed: %1").arg(qstr(result.error.message));
        } else {
            currentPreview_ = result.value;
        }
        updatePreviewLabels();
        updateInlineMessage();
        updateExecuteState();
    });

    connect(&executeWatcher_, &QFutureWatcher<ExecuteResult>::finished, this, [this]() {
        executeInFlight_ = false;
        const auto result = executeWatcher_.result();
        if (!result.ok) {
            savor::db::archive::ArchiveOperationProgress failed{};
            failed.phase = savor::db::archive::ArchiveOperationPhase::Failed;
            failed.message = result.error.message;
            failed.indeterminate = false;
            applyArchiveProgress(failed);
            executeStatusLabel_->setText(QStringLiteral("Failed: %1").arg(qstr(result.error.message)));
            postStatusMessage(executeStatusLabel_->text(), StatusToast::Severity::Error);
        } else {
            savor::db::archive::ArchiveOperationProgress complete{};
            complete.phase = savor::db::archive::ArchiveOperationPhase::Complete;
            complete.message = "Archive operation complete";
            complete.completed_units = 1;
            complete.total_units = 1;
            complete.indeterminate = false;
            applyArchiveProgress(complete);
            executeStatusLabel_->setText(QStringLiteral("Package %1: %2")
                .arg(static_cast<qint64>(result.value.archive_package_id))
                .arg(QString::fromStdString(result.value.package_root.string())));
            postStatusMessage(QStringLiteral("Archive package created."), StatusToast::Severity::Info);
            refreshCandidates();
        }
        setBusy(false);
        requestPreview();
    });
}

void ArchiveWorkbenchPage::refreshCandidates()
{
    if (!pageActive_ || candidateRefreshPipeline_ == nullptr) return;
    candidateRefreshPipeline_->requestRefresh(savorqt::gui::RefreshReason::Manual);
}

void ArchiveWorkbenchPage::requestPreview()
{
    if (!pageActive_ || previewRefreshPipeline_ == nullptr) return;
    previewRefreshPipeline_->requestRefresh(savorqt::gui::RefreshReason::Manual);
}

void ArchiveWorkbenchPage::executeArchive()
{
    if (!currentPreview_.has_value() || currentPreview_->selection.selected_count <= 0) return;
    const auto expected = QString::number(currentPreview_->selection.selected_count);
    if (confirmationEdit_->text().trimmed() != expected) {
        executeStatusLabel_->setText(QStringLiteral("Type %1 in the confirmation field before creating the archive.").arg(expected));
        postStatusMessage(executeStatusLabel_->text(), StatusToast::Severity::Warn);
        return;
    }
    executeInFlight_ = true;
    setBusy(true);
    resetArchiveProgress();
    savorqt::db::ArchiveWorkflowExecuteRequest request{};
    request.selection = currentPreview_->selection.selection;
    request.purge_after_verify = purgeAfterVerifyCheck_->isChecked() && !packageOnlyCheck_->isChecked();
    request.trace_id = "qt-archive-workbench";
    auto archiveName = archiveNameEdit_->text().trimmed();
    if (archiveName.isEmpty()) {
        archiveName = fallbackArchiveName();
        archiveNameEdit_->setText(archiveName);
    }
    request.archive_name = archiveName.toStdString();
    const auto archiveNotes = archiveNotesEdit_->toPlainText().trimmed();
    if (!archiveNotes.isEmpty()) {
        request.archive_notes = archiveNotes.toStdString();
    }
    QPointer<ArchiveWorkbenchPage> self(this);
    request.progress_sink = [self](const savor::db::archive::ArchiveOperationProgress& progress) {
        if (self.isNull()) {
            return;
        }
        QMetaObject::invokeMethod(
            self.data(),
            [self, progress]() {
                if (!self.isNull()) {
                    self->applyArchiveProgress(progress);
                }
            },
            Qt::QueuedConnection);
    };
    executeWatcher_.setFuture(QtConcurrent::run([request]() {
        return savorqt::db::SavorDbArchiveService::ExecuteWorkflowArchive(request);
    }));
}

void ArchiveWorkbenchPage::applyCandidateRows(const std::vector<savorqt::db::ArchiveCandidateRow>& rows)
{
    currentRows_.clear();
    for (const auto& row : rows) {
        CandidateRow display{};
        display.row = row;
        display.excluded = explicitExclusions_.find(row.workflow_instance_id) != explicitExclusions_.end();
        display.selected = !display.excluded && (includeFilterMatches_ || explicitIncludes_.find(row.workflow_instance_id) != explicitIncludes_.end());
        currentRows_.push_back(std::move(display));
    }

    QSignalBlocker blocker(candidateTable_);
    candidateTable_->setRowCount(static_cast<int>(currentRows_.size()));
    for (int r = 0; r < static_cast<int>(currentRows_.size()); ++r) {
        const auto& item = currentRows_[static_cast<std::size_t>(r)];
        auto* include = makeItem(QString());
        include->setFlags((include->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        include->setCheckState(item.selected ? Qt::Checked : Qt::Unchecked);
        include->setData(kWorkflowIdRole, static_cast<qint64>(item.row.workflow_instance_id));
        candidateTable_->setItem(r, 0, include);
        candidateTable_->setItem(r, 1, makeItem(QString::number(static_cast<qint64>(item.row.workflow_instance_id))));
        candidateTable_->setItem(r, 2, makeItem(qstr(item.row.workflow_kind)));
        candidateTable_->setItem(r, 3, makeItem(qstr(item.row.display_state)));
        candidateTable_->setItem(r, 4, makeItem(formatTime(item.row.created_at_utc)));
        candidateTable_->setItem(r, 5, makeItem(formatOptionalTime(item.row.completed_at_utc)));
        candidateTable_->setItem(r, 6, makeItem(QString::number(static_cast<qint64>(item.row.failed_step_count))));
        candidateTable_->setItem(r, 7, makeItem(QString::number(static_cast<qint64>(item.row.blocked_step_count))));
        candidateTable_->setItem(r, 8, makeItem(QString::number(static_cast<qint64>(item.row.battle_final_victory_count))));
        candidateTable_->setItem(r, 9, makeItem(item.excluded ? QStringLiteral("Excluded") : (item.selected ? QStringLiteral("Selected") : QStringLiteral("-"))));
    }
    updateInlineMessage();
}

void ArchiveWorkbenchPage::updatePreviewLabels()
{
    if (previewFetchInFlight_) {
        previewLabel_->setText(QStringLiteral("Preview loading..."));
        return;
    }
    if (!currentPreview_.has_value()) {
        previewLabel_->setText(QStringLiteral("No preview."));
        blockersLabel_->clear();
        return;
    }
    const auto& selection = currentPreview_->selection;
    const auto& preview = currentPreview_->preview.preview;
    previewLabel_->setText(QStringLiteral(
        "Selected: %1\nExcluded: %2\nExecution rows: %3\nAnalysis rows: %4\nUIRead rows: %5\nSAV rows: %6\nShared SAVs: %7\nExclusive SAVs: %8\nSAV bytes: %9")
        .arg(selection.selected_count)
        .arg(selection.excluded_count)
        .arg(preview.execution_row_count)
        .arg(preview.analysis_row_count)
        .arg(preview.ui_read_snapshot_row_count)
        .arg(preview.savestate_count)
        .arg(preview.shared_savestate_count)
        .arg(preview.exclusive_savestate_count)
        .arg(static_cast<qulonglong>(preview.savestate_bytes)));
    blockersLabel_->setText(joinMessages(preview.purge_blockers));
}

void ArchiveWorkbenchPage::updateExecuteState()
{
    const int selected = currentPreview_.has_value() ? currentPreview_->selection.selected_count : 0;
    executeButton_->setEnabled(!executeInFlight_ && !previewFetchInFlight_ && selected > 0);
    if (!inlineMessage_.isEmpty()) {
        return;
    }
    if (executeInFlight_) {
        executeStatusLabel_->setText(QStringLiteral("Archive operation running..."));
    } else if (previewFetchInFlight_) {
        executeStatusLabel_->setText(QStringLiteral("Waiting for archive preview..."));
    } else if (selected <= 0) {
        executeStatusLabel_->setText(QStringLiteral("Select workflows to create an archive."));
    } else if (confirmationEdit_->text().trimmed() != QString::number(selected)) {
        executeStatusLabel_->setText(QStringLiteral("Type %1 to confirm %1 selected workflows.").arg(selected));
    } else {
        executeStatusLabel_->setText(purgeAfterVerifyCheck_->isChecked()
            ? QStringLiteral("Ready to create, verify, and purge.")
            : QStringLiteral("Ready to create package only."));
    }
}

void ArchiveWorkbenchPage::updateInlineMessage()
{
    statusLabel_->setText(QStringLiteral("Rows: %1 | Explicit includes: %2 | Exclusions: %3%4")
        .arg(currentRows_.size())
        .arg(explicitIncludes_.size())
        .arg(explicitExclusions_.size())
        .arg(includeFilterMatches_ ? QStringLiteral(" | all matching") : QString()));
    if (!inlineMessage_.isEmpty()) {
        executeStatusLabel_->setText(inlineMessage_);
    }
}

void ArchiveWorkbenchPage::setBusy(bool busy)
{
    for (auto* widget : { applyButton_, refreshButton_, selectVisibleButton_, clearVisibleButton_, selectAllMatchingButton_, excludeSelectedButton_, clearExclusionsButton_, executeButton_ }) {
        widget->setEnabled(!busy);
    }
    archiveNameEdit_->setEnabled(!busy);
    archiveNotesEdit_->setEnabled(!busy);
    candidateTable_->setEnabled(!busy);
    updateExecuteState();
}

void ArchiveWorkbenchPage::resetArchiveProgress()
{
    archiveProgressPhaseLabel_->setText(QStringLiteral("Starting archive operation..."));
    archiveProgressBar_->setRange(0, 0);
    archiveProgressBar_->setValue(0);
}

void ArchiveWorkbenchPage::applyArchiveProgress(const savor::db::archive::ArchiveOperationProgress& progress)
{
    const auto phase = progressPhaseText(progress.phase);
    const auto message = qstr(progress.message);
    QString text = message.isEmpty() ? phase : QStringLiteral("%1: %2").arg(phase, message);
    if (!progress.indeterminate && progress.total_units > 0) {
        text += QStringLiteral(" (%1/%2)").arg(static_cast<qint64>(progress.completed_units)).arg(static_cast<qint64>(progress.total_units));
    }
    if (progress.bytes_completed.has_value() && progress.bytes_total.has_value() && *progress.bytes_total > 0) {
        text += QStringLiteral(" | %1/%2 bytes")
            .arg(static_cast<qulonglong>(*progress.bytes_completed))
            .arg(static_cast<qulonglong>(*progress.bytes_total));
    }
    archiveProgressPhaseLabel_->setText(text);

    if (progress.phase == savor::db::archive::ArchiveOperationPhase::Failed) {
        archiveProgressBar_->setRange(0, 1);
        archiveProgressBar_->setValue(0);
        return;
    }
    if (progress.indeterminate || progress.total_units <= 0) {
        archiveProgressBar_->setRange(0, 0);
        return;
    }
    archiveProgressBar_->setRange(0, static_cast<int>(std::min<std::int64_t>(progress.total_units, std::numeric_limits<int>::max())));
    archiveProgressBar_->setValue(static_cast<int>(std::clamp<std::int64_t>(progress.completed_units, 0, archiveProgressBar_->maximum())));
}

QString ArchiveWorkbenchPage::fallbackArchiveName() const
{
    const auto timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    return qstr(savorqt::db::SavorDbArchiveService::BuildFallbackArchiveName(
        currentFilter(),
        currentPreview_.has_value() ? currentPreview_->selection.selected_count : 0,
        timestamp.toStdString()));
}

void ArchiveWorkbenchPage::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    emit statusToastRequested(StatusToast{ severity, text, QString(), 1, QDateTime::currentDateTimeUtc(), 4000 });
}

savorqt::db::ArchiveWorkflowFilter ArchiveWorkbenchPage::currentFilter() const
{
    savorqt::db::ArchiveWorkflowFilter filter{};
    filter.display_state = displayStateFilter_->currentData().toString().trimmed().toStdString();
    filter.workflow_kind = workflowKindFilter_->text().trimmed().toStdString();
    filter.final_victory_mode = static_cast<savorqt::db::ArchiveFinalVictoryMode>(finalVictoryFilter_->currentData().toInt());
    filter.problem_mode = static_cast<savorqt::db::ArchiveProblemMode>(problemFilter_->currentData().toInt());
    filter.text_filter = textFilter_->text().trimmed().toStdString();
    if (createdFromEnabled_->isChecked()) filter.created_from_utc = createdFromEdit_->dateTime().toMSecsSinceEpoch();
    if (createdToEnabled_->isChecked()) filter.created_to_utc = createdToEdit_->dateTime().toMSecsSinceEpoch();
    if (completedFromEnabled_->isChecked()) filter.completed_from_utc = completedFromEdit_->dateTime().toMSecsSinceEpoch();
    if (completedToEnabled_->isChecked()) filter.completed_to_utc = completedToEdit_->dateTime().toMSecsSinceEpoch();
    return filter;
}

savorqt::db::ArchiveSelectionBuildRequest ArchiveWorkbenchPage::currentSelectionRequest() const
{
    savorqt::db::ArchiveSelectionBuildRequest request{};
    request.filter = currentFilter();
    request.include_filter_matches = includeFilterMatches_;
    request.explicit_includes = toVector(explicitIncludes_);
    request.explicit_exclusions = toVector(explicitExclusions_);
    return request;
}

std::vector<std::int64_t> ArchiveWorkbenchPage::visibleWorkflowIds() const
{
    std::vector<std::int64_t> ids;
    for (const auto& row : currentRows_) ids.push_back(row.row.workflow_instance_id);
    return ids;
}

std::vector<std::int64_t> ArchiveWorkbenchPage::selectedHighlightedWorkflowIds() const
{
    std::set<std::int64_t> ids;
    for (const auto* item : candidateTable_->selectedItems()) {
        const int row = item->row();
        if (row >= 0 && row < static_cast<int>(currentRows_.size())) {
            ids.insert(currentRows_[static_cast<std::size_t>(row)].row.workflow_instance_id);
        }
    }
    return toVector(ids);
}

void ArchiveWorkbenchPage::setVisibleRowsSelected(bool selected)
{
    for (const auto id : visibleWorkflowIds()) {
        if (selected) {
            explicitExclusions_.erase(id);
            explicitIncludes_.insert(id);
        } else {
            explicitIncludes_.erase(id);
            if (includeFilterMatches_) explicitExclusions_.insert(id);
        }
    }
    requestPreview();
    refreshCandidates();
}

void ArchiveWorkbenchPage::markHighlightedRowsExcluded()
{
    for (const auto id : selectedHighlightedWorkflowIds()) {
        explicitIncludes_.erase(id);
        explicitExclusions_.insert(id);
    }
    requestPreview();
    refreshCandidates();
}

void ArchiveWorkbenchPage::clearSelectionState()
{
    includeFilterMatches_ = false;
    explicitIncludes_.clear();
    explicitExclusions_.clear();
    requestPreview();
    refreshCandidates();
}
