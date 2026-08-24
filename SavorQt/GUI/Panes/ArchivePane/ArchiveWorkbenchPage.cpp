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
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <limits>
#include <sstream>

namespace {
constexpr int kWorkflowIdRole = Qt::UserRole + 1;
constexpr int kArchivePackageIdRole = Qt::UserRole + 2;
constexpr int kRehydrateRequestIdRole = Qt::UserRole + 3;

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

QString packageDisplayName(const savor::db::UiArchiveCatalogRow& row)
{
    if (!row.archive_name.empty()) {
        return qstr(row.archive_name);
    }
    return QStringLiteral("Package %1").arg(static_cast<qint64>(row.archive_package_id));
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

bool isInactiveRehydrateStatus(const std::string& status)
{
    return status == "COMPLETED" || status == "FAILED";
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
    if (rehydratePackageRefreshPipeline_ != nullptr) {
        rehydratePackageRefreshPipeline_->setActive(active);
    }
    if (rehydratePreviewRefreshPipeline_ != nullptr) {
        rehydratePreviewRefreshPipeline_->setActive(active);
    }
    if (rehydrateRequestRefreshPipeline_ != nullptr) {
        rehydrateRequestRefreshPipeline_->setActive(active);
    }
    if (active) {
        refreshCandidates();
        refreshArchivePackages();
    }
}

void ArchiveWorkbenchPage::createWidgets()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    tabWidget_ = new QTabWidget(this);
    auto* archiveTab = new QWidget(tabWidget_);
    auto* archiveRoot = new QVBoxLayout(archiveTab);
    archiveRoot->setContentsMargins(0, 0, 0, 0);
    archiveRoot->setSpacing(10);

    auto* filterPanel = new QFrame(archiveTab);
    filterPanel->setObjectName("jobSetsToolbarPanel");
    auto* filterLayout = new QGridLayout(filterPanel);
    filterLayout->setContentsMargins(8, 7, 8, 7);
    filterLayout->setHorizontalSpacing(10);
    filterLayout->setVerticalSpacing(8);

    displayStateFilter_ = new QComboBox(filterPanel);
    displayStateFilter_->setObjectName("jobSetsFilterCombo");
    displayStateFilter_->addItem(QStringLiteral("All final states"), QString());
    displayStateFilter_->addItem(QStringLiteral("Completed"), QStringLiteral("COMPLETED"));
    displayStateFilter_->addItem(QStringLiteral("Canceled"), QStringLiteral("CANCELED"));

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
    archiveRoot->addWidget(filterPanel);

    auto* actionsPanel = new QFrame(archiveTab);
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
    archiveRoot->addWidget(actionsPanel);

    auto* splitter = new QSplitter(Qt::Horizontal, archiveTab);
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
    archiveRoot->addWidget(splitter, 1);

    tabWidget_->addTab(archiveTab, QStringLiteral("Create Archive"));
    auto* rehydrateTab = new QWidget(tabWidget_);
    createRehydrateWidgets(rehydrateTab);
    tabWidget_->addTab(rehydrateTab, QStringLiteral("Rehydrate Archive"));
    root->addWidget(tabWidget_, 1);

    candidateRefreshPipeline_ = new savorqt::gui::AsyncRefreshPipeline<CandidateRefreshRequest, CandidateResult>(this);
    previewRefreshPipeline_ = new savorqt::gui::AsyncRefreshPipeline<PreviewRefreshRequest, PreviewResult>(this);
    rehydratePackageRefreshPipeline_ = new savorqt::gui::AsyncRefreshPipeline<RehydratePackageRefreshRequest, RehydratePackageResult>(this);
    rehydratePreviewRefreshPipeline_ = new savorqt::gui::AsyncRefreshPipeline<RehydratePreviewRefreshRequest, RehydratePreviewResult>(this);
    rehydrateRequestRefreshPipeline_ = new savorqt::gui::AsyncRefreshPipeline<RehydrateRequestRefreshRequest, RehydrateRequestResult>(this);
}

void ArchiveWorkbenchPage::createRehydrateWidgets(QWidget* tab)
{
    auto* root = new QVBoxLayout(tab);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    auto* filterPanel = new QFrame(tab);
    filterPanel->setObjectName("jobSetsToolbarPanel");
    auto* filterLayout = new QGridLayout(filterPanel);
    filterLayout->setContentsMargins(8, 7, 8, 7);
    filterLayout->setHorizontalSpacing(10);
    filterLayout->setVerticalSpacing(8);

    rehydrateSearchEdit_ = new QLineEdit(filterPanel);
    rehydrateSearchEdit_->setObjectName("jobSetsFilterCombo");
    rehydrateSearchEdit_->setPlaceholderText(QStringLiteral("id/name/notes"));

    rehydrateScopeFilter_ = new QComboBox(filterPanel);
    rehydrateScopeFilter_->setObjectName("jobSetsFilterCombo");
    rehydrateScopeFilter_->addItem(QStringLiteral("Any scope"), QString());
    rehydrateScopeFilter_->addItem(QStringLiteral("Workflow selection"), QStringLiteral("workflow_selection"));
    rehydrateScopeFilter_->addItem(QStringLiteral("Job set"), QStringLiteral("job_set"));

    rehydrateChecksumFilter_ = new QComboBox(filterPanel);
    rehydrateChecksumFilter_->setObjectName("jobSetsFilterCombo");
    rehydrateChecksumFilter_->addItem(QStringLiteral("Any checksum"), QString());
    rehydrateChecksumFilter_->addItem(QStringLiteral("Pass"), QStringLiteral("PASS"));
    rehydrateChecksumFilter_->addItem(QStringLiteral("Pending"), QStringLiteral("PENDING"));
    rehydrateChecksumFilter_->addItem(QStringLiteral("Failed"), QStringLiteral("FAILED"));

    rehydrateWorkflowOnlyCheck_ = new QCheckBox(QStringLiteral("Workflow packages only"), filterPanel);
    rehydrateWorkflowOnlyCheck_->setChecked(true);

    const auto now = QDateTime::currentDateTime();
    rehydrateCreatedFromEnabled_ = new QCheckBox(QStringLiteral("Created from"), filterPanel);
    rehydrateCreatedToEnabled_ = new QCheckBox(QStringLiteral("Created to"), filterPanel);
    rehydrateCreatedFromEdit_ = new QDateTimeEdit(now.addDays(-30), filterPanel);
    rehydrateCreatedToEdit_ = new QDateTimeEdit(now, filterPanel);
    for (auto* edit : { rehydrateCreatedFromEdit_, rehydrateCreatedToEdit_ }) {
        edit->setObjectName("jobSetsFilterCombo");
        edit->setCalendarPopup(true);
        edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
    }

    rehydrateRefreshButton_ = new QPushButton(QStringLiteral("Refresh"), filterPanel);
    rehydrateRefreshButton_->setObjectName("jobSetsPrimaryButton");

    filterLayout->addWidget(new QLabel(QStringLiteral("Search"), filterPanel), 0, 0);
    filterLayout->addWidget(rehydrateSearchEdit_, 1, 0);
    filterLayout->addWidget(new QLabel(QStringLiteral("Scope"), filterPanel), 0, 1);
    filterLayout->addWidget(rehydrateScopeFilter_, 1, 1);
    filterLayout->addWidget(new QLabel(QStringLiteral("Checksum"), filterPanel), 0, 2);
    filterLayout->addWidget(rehydrateChecksumFilter_, 1, 2);
    filterLayout->addWidget(rehydrateWorkflowOnlyCheck_, 1, 3);
    filterLayout->addWidget(rehydrateRefreshButton_, 1, 4);
    filterLayout->addWidget(rehydrateCreatedFromEnabled_, 2, 0);
    filterLayout->addWidget(rehydrateCreatedFromEdit_, 3, 0);
    filterLayout->addWidget(rehydrateCreatedToEnabled_, 2, 1);
    filterLayout->addWidget(rehydrateCreatedToEdit_, 3, 1);
    filterLayout->setColumnStretch(5, 1);
    root->addWidget(filterPanel);

    auto* metaPanel = new QFrame(tab);
    metaPanel->setObjectName("jobSetsPagingPanel");
    auto* metaLayout = new QHBoxLayout(metaPanel);
    metaLayout->setContentsMargins(8, 7, 8, 7);
    rehydrateCatalogStatusLabel_ = new QLabel(metaPanel);
    rehydrateCatalogStatusLabel_->setObjectName("jobSetsMetaText");
    metaLayout->addWidget(rehydrateCatalogStatusLabel_);
    metaLayout->addStretch();
    root->addWidget(metaPanel);

    auto* splitter = new QSplitter(Qt::Horizontal, tab);
    rehydratePackageTable_ = new QTableWidget(splitter);
    rehydratePackageTable_->setObjectName("jobSetsTreeView");
    rehydratePackageTable_->setColumnCount(7);
    rehydratePackageTable_->setHorizontalHeaderLabels({
        QStringLiteral("Package"),
        QStringLiteral("Name"),
        QStringLiteral("Workflows"),
        QStringLiteral("Scope"),
        QStringLiteral("Created"),
        QStringLiteral("Checksum"),
        QStringLiteral("Last request"),
    });
    rehydratePackageTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    rehydratePackageTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    rehydratePackageTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    rehydratePackageTable_->setAlternatingRowColors(true);
    rehydratePackageTable_->verticalHeader()->hide();
    rehydratePackageTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    rehydratePackageTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    rehydratePackageTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    rehydratePackageTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    rehydratePackageTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Interactive);
    rehydratePackageTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Interactive);
    rehydratePackageTable_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Interactive);
    splitter->addWidget(rehydratePackageTable_);

    auto* sidePanel = new QWidget(splitter);
    auto* sideLayout = new QVBoxLayout(sidePanel);
    sideLayout->setContentsMargins(8, 0, 0, 0);

    rehydratePreviewLabel_ = new QLabel(sidePanel);
    rehydratePreviewLabel_->setObjectName("jobSetsMetaText");
    rehydratePreviewLabel_->setWordWrap(true);
    rehydrateBlockersLabel_ = new QLabel(sidePanel);
    rehydrateBlockersLabel_->setObjectName("jobSetsInlineMessage");
    rehydrateBlockersLabel_->setWordWrap(true);

    rehydrateNamespaceEdit_ = new QLineEdit(sidePanel);
    rehydrateNamespaceEdit_->setPlaceholderText(QStringLiteral("target namespace"));
    rehydrateConfirmationEdit_ = new QLineEdit(sidePanel);
    rehydrateConfirmationEdit_->setPlaceholderText(QStringLiteral("archive package id"));
    rehydrateExecuteButton_ = new QPushButton(QStringLiteral("Rehydrate archive"), sidePanel);
    rehydrateExecuteButton_->setObjectName("jobSetsPrimaryButton");
    rehydrateProgressPhaseLabel_ = new QLabel(QStringLiteral("Idle"), sidePanel);
    rehydrateProgressPhaseLabel_->setObjectName("jobSetsMetaText");
    rehydrateProgressPhaseLabel_->setWordWrap(true);
    rehydrateProgressBar_ = new QProgressBar(sidePanel);
    rehydrateProgressBar_->setRange(0, 1);
    rehydrateProgressBar_->setValue(0);
    rehydrateProgressBar_->setTextVisible(true);
    rehydrateStatusLabel_ = new QLabel(sidePanel);
    rehydrateStatusLabel_->setObjectName("jobSetsMetaText");
    rehydrateStatusLabel_->setWordWrap(true);

    rehydrateRequestTable_ = new QTableWidget(sidePanel);
    rehydrateRequestTable_->setObjectName("jobSetsTreeView");
    rehydrateRequestTable_->setColumnCount(5);
    rehydrateRequestTable_->setHorizontalHeaderLabels({
        QStringLiteral("Request"),
        QStringLiteral("Status"),
        QStringLiteral("Namespace"),
        QStringLiteral("Requested"),
        QStringLiteral("Error"),
    });
    rehydrateRequestTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    rehydrateRequestTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    rehydrateRequestTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    rehydrateRequestTable_->setAlternatingRowColors(true);
    rehydrateRequestTable_->verticalHeader()->hide();
    rehydrateRequestTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    rehydrateRequestTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    rehydrateRequestTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    rehydrateRequestTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    rehydrateRequestTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    rehydrateCleanupButton_ = new QPushButton(QStringLiteral("Cleanup selected request"), sidePanel);

    sideLayout->addWidget(new QLabel(QStringLiteral("Preview"), sidePanel));
    sideLayout->addWidget(rehydratePreviewLabel_);
    sideLayout->addWidget(rehydrateBlockersLabel_);
    sideLayout->addSpacing(10);
    sideLayout->addWidget(new QLabel(QStringLiteral("Target namespace"), sidePanel));
    sideLayout->addWidget(rehydrateNamespaceEdit_);
    sideLayout->addWidget(rehydrateConfirmationEdit_);
    sideLayout->addWidget(rehydrateExecuteButton_);
    sideLayout->addWidget(rehydrateProgressPhaseLabel_);
    sideLayout->addWidget(rehydrateProgressBar_);
    sideLayout->addWidget(rehydrateStatusLabel_);
    sideLayout->addSpacing(10);
    sideLayout->addWidget(new QLabel(QStringLiteral("Request history"), sidePanel));
    sideLayout->addWidget(rehydrateRequestTable_, 1);
    sideLayout->addWidget(rehydrateCleanupButton_);
    splitter->addWidget(sidePanel);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    root->addWidget(splitter, 1);

    rehydratePreviewLabel_->setText(QStringLiteral("Select an archive package."));
    rehydrateCatalogStatusLabel_->setText(QStringLiteral("Rows: 0"));
    updateRehydrateExecuteState();
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

    connect(rehydrateRefreshButton_, &QPushButton::clicked, this, &ArchiveWorkbenchPage::refreshArchivePackages);
    connect(rehydrateSearchEdit_, &QLineEdit::returnPressed, this, &ArchiveWorkbenchPage::refreshArchivePackages);
    connect(rehydrateScopeFilter_, &QComboBox::currentIndexChanged, this, [this]() { refreshArchivePackages(); });
    connect(rehydrateChecksumFilter_, &QComboBox::currentIndexChanged, this, [this]() { refreshArchivePackages(); });
    connect(rehydrateWorkflowOnlyCheck_, &QCheckBox::toggled, this, [this]() { refreshArchivePackages(); });
    connect(rehydrateCreatedFromEnabled_, &QCheckBox::toggled, this, [this]() { refreshArchivePackages(); });
    connect(rehydrateCreatedToEnabled_, &QCheckBox::toggled, this, [this]() { refreshArchivePackages(); });
    connect(rehydrateNamespaceEdit_, &QLineEdit::textChanged, this, [this]() {
        requestRehydratePreview();
        updateRehydrateExecuteState();
    });
    connect(rehydrateConfirmationEdit_, &QLineEdit::textChanged, this, &ArchiveWorkbenchPage::updateRehydrateExecuteState);
    connect(rehydrateExecuteButton_, &QPushButton::clicked, this, &ArchiveWorkbenchPage::executeRehydrate);
    connect(rehydrateCleanupButton_, &QPushButton::clicked, this, &ArchiveWorkbenchPage::cleanupSelectedRehydrateRequest);
    connect(rehydratePackageTable_, &QTableWidget::itemSelectionChanged, this, [this]() {
        std::int64_t packageId = 0;
        const auto selectedItems = rehydratePackageTable_->selectedItems();
        if (!selectedItems.empty()) {
            const int row = selectedItems.front()->row();
            if (auto* idItem = rehydratePackageTable_->item(row, 0)) {
                packageId = idItem->data(kArchivePackageIdRole).toLongLong();
            }
        }
        selectedArchivePackageId_ = packageId;
        const auto selected = selectedArchivePackage();
        currentRehydratePreview_.reset();
        if (selected.has_value()) {
            rehydrateNamespaceEdit_->setText(fallbackRehydrateNamespace());
        } else {
            applyRehydrateRequestRows({});
        }
        refreshRehydrateRequests();
        requestRehydratePreview();
        updateRehydratePreviewLabels();
        updateRehydrateExecuteState();
    });
    connect(rehydrateRequestTable_, &QTableWidget::itemSelectionChanged, this, &ArchiveWorkbenchPage::updateRehydrateExecuteState);

    rehydratePackageRefreshPipeline_->setAutoRefreshEnabled(false);
    rehydratePackageRefreshPipeline_->setRequestBuilder([this](savorqt::gui::RefreshReason) -> std::optional<RehydratePackageRefreshRequest> {
        rehydratePackageFetchInFlight_ = true;
        rehydrateCatalogStatusLabel_->setText(QStringLiteral("Loading archive packages..."));
        return RehydratePackageRefreshRequest{ .filter = currentArchivePackageFilter() };
    });
    rehydratePackageRefreshPipeline_->setLoadAndPrepare([](RehydratePackageRefreshRequest request) {
        const auto packages = savorqt::db::SavorDbArchiveService::ListArchivePackages(request.filter);
        if (!packages.ok) {
            return savorqt::gui::AsyncRefreshResult<RehydratePackageResult>::Ok(RehydratePackageResult::Err(packages.error));
        }
        RehydratePackageRefreshData data{};
        data.packages = packages.value;
        return savorqt::gui::AsyncRefreshResult<RehydratePackageResult>::Ok(RehydratePackageResult::Ok(std::move(data)));
    });
    rehydratePackageRefreshPipeline_->setApply([this](const RehydratePackageResult& result, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        rehydratePackageFetchInFlight_ = false;
        if (!result.ok) {
            rehydrateInlineMessage_ = QStringLiteral("Package refresh failed: %1").arg(qstr(result.error.message));
            applyArchivePackageRows({});
        } else {
            rehydrateInlineMessage_.clear();
            applyArchivePackageRows(result.value.packages.rows);
        }
        refreshRehydrateRequests();
        requestRehydratePreview();
        updateRehydratePreviewLabels();
        updateRehydrateExecuteState();
    });

    rehydratePreviewRefreshPipeline_->setAutoRefreshEnabled(false);
    rehydratePreviewRefreshPipeline_->setRequestBuilder([this](savorqt::gui::RefreshReason) -> std::optional<RehydratePreviewRefreshRequest> {
        if (selectedArchivePackageId_ <= 0 || rehydrateNamespaceEdit_->text().trimmed().isEmpty()) {
            return std::nullopt;
        }
        rehydratePreviewFetchInFlight_ = true;
        updateRehydratePreviewLabels();
        return RehydratePreviewRefreshRequest{
            .archive_package_id = selectedArchivePackageId_,
            .target_namespace = rehydrateNamespaceEdit_->text().trimmed().toStdString(),
        };
    });
    rehydratePreviewRefreshPipeline_->setLoadAndPrepare([](RehydratePreviewRefreshRequest request) {
        const auto preview = savorqt::db::SavorDbArchiveService::PreviewRehydrate(request.archive_package_id, request.target_namespace);
        if (!preview.ok) {
            return savorqt::gui::AsyncRefreshResult<RehydratePreviewResult>::Ok(RehydratePreviewResult::Err(preview.error));
        }
        RehydratePreviewRefreshData data{};
        data.preview = preview.value;
        return savorqt::gui::AsyncRefreshResult<RehydratePreviewResult>::Ok(RehydratePreviewResult::Ok(std::move(data)));
    });
    rehydratePreviewRefreshPipeline_->setApply([this](const RehydratePreviewResult& result, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        rehydratePreviewFetchInFlight_ = false;
        if (!result.ok) {
            currentRehydratePreview_.reset();
            rehydrateInlineMessage_ = QStringLiteral("Rehydrate preview failed: %1").arg(qstr(result.error.message));
        } else {
            currentRehydratePreview_ = result.value;
            if (result.value.preview.success) {
                rehydrateInlineMessage_.clear();
            }
        }
        updateRehydratePreviewLabels();
        updateRehydrateExecuteState();
    });

    rehydrateRequestRefreshPipeline_->setAutoRefreshEnabled(false);
    rehydrateRequestRefreshPipeline_->setRequestBuilder([this](savorqt::gui::RefreshReason) -> std::optional<RehydrateRequestRefreshRequest> {
        if (selectedArchivePackageId_ <= 0) {
            return std::nullopt;
        }
        rehydrateRequestFetchInFlight_ = true;
        return RehydrateRequestRefreshRequest{ .archive_package_id = selectedArchivePackageId_ };
    });
    rehydrateRequestRefreshPipeline_->setLoadAndPrepare([](RehydrateRequestRefreshRequest request) {
        const auto requests = savorqt::db::SavorDbArchiveService::ListRehydrateRequests(request.archive_package_id);
        if (!requests.ok) {
            return savorqt::gui::AsyncRefreshResult<RehydrateRequestResult>::Ok(RehydrateRequestResult::Err(requests.error));
        }
        RehydrateRequestRefreshData data{};
        data.requests = requests.value;
        return savorqt::gui::AsyncRefreshResult<RehydrateRequestResult>::Ok(RehydrateRequestResult::Ok(std::move(data)));
    });
    rehydrateRequestRefreshPipeline_->setApply([this](const RehydrateRequestResult& result, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        rehydrateRequestFetchInFlight_ = false;
        if (!result.ok) {
            rehydrateInlineMessage_ = QStringLiteral("Request history refresh failed: %1").arg(qstr(result.error.message));
            applyRehydrateRequestRows({});
        } else {
            applyRehydrateRequestRows(result.value.requests.rows);
        }
        updateRehydrateExecuteState();
    });

    connect(&rehydrateExecuteWatcher_, &QFutureWatcher<RehydrateExecuteResult>::finished, this, [this]() {
        rehydrateExecuteInFlight_ = false;
        const auto result = rehydrateExecuteWatcher_.result();
        if (!result.ok) {
            savor::db::archive::ArchiveOperationProgress failed{};
            failed.phase = savor::db::archive::ArchiveOperationPhase::Failed;
            failed.message = result.error.message;
            failed.indeterminate = false;
            applyRehydrateProgress(failed);
            rehydrateStatusLabel_->setText(QStringLiteral("Failed: %1").arg(qstr(result.error.message)));
            postStatusMessage(rehydrateStatusLabel_->text(), StatusToast::Severity::Error);
        } else {
            savor::db::archive::ArchiveOperationProgress complete{};
            complete.phase = savor::db::archive::ArchiveOperationPhase::Complete;
            complete.message = "Rehydrate operation complete";
            complete.completed_units = 1;
            complete.total_units = 1;
            complete.indeterminate = false;
            applyRehydrateProgress(complete);
            const auto requestId = result.value.request_ids.empty() ? 0 : result.value.request_ids.front();
            rehydrateStatusLabel_->setText(QStringLiteral("Rehydrate request %1 completed.").arg(static_cast<qint64>(requestId)));
            postStatusMessage(QStringLiteral("Archive rehydrated."), StatusToast::Severity::Info);
        }
        setRehydrateBusy(false);
        refreshRehydrateRequests();
        requestRehydratePreview();
    });

    connect(&rehydrateCleanupWatcher_, &QFutureWatcher<RehydrateCleanupResult>::finished, this, [this]() {
        rehydrateCleanupInFlight_ = false;
        const auto result = rehydrateCleanupWatcher_.result();
        if (!result.ok) {
            rehydrateStatusLabel_->setText(QStringLiteral("Cleanup failed: %1").arg(qstr(result.error.message)));
            postStatusMessage(rehydrateStatusLabel_->text(), StatusToast::Severity::Error);
        } else {
            rehydrateStatusLabel_->setText(QStringLiteral("Rehydrate request cleanup complete."));
            postStatusMessage(rehydrateStatusLabel_->text(), StatusToast::Severity::Info);
        }
        setRehydrateBusy(false);
        refreshRehydrateRequests();
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

void ArchiveWorkbenchPage::refreshArchivePackages()
{
    if (!pageActive_ || rehydratePackageRefreshPipeline_ == nullptr) return;
    rehydratePackageRefreshPipeline_->requestRefresh(savorqt::gui::RefreshReason::Manual);
}

void ArchiveWorkbenchPage::refreshRehydrateRequests()
{
    if (!pageActive_ || rehydrateRequestRefreshPipeline_ == nullptr || selectedArchivePackageId_ <= 0) return;
    rehydrateRequestRefreshPipeline_->requestRefresh(savorqt::gui::RefreshReason::Manual);
}

void ArchiveWorkbenchPage::requestRehydratePreview()
{
    if (!pageActive_ || rehydratePreviewRefreshPipeline_ == nullptr || selectedArchivePackageId_ <= 0) return;
    rehydratePreviewRefreshPipeline_->requestRefresh(savorqt::gui::RefreshReason::Manual);
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

void ArchiveWorkbenchPage::executeRehydrate()
{
    const auto selected = selectedArchivePackage();
    if (!selected.has_value() || !currentRehydratePreview_.has_value()) return;
    const auto expected = QString::number(static_cast<qint64>(selected->archive_package_id));
    if (rehydrateConfirmationEdit_->text().trimmed() != expected) {
        rehydrateStatusLabel_->setText(QStringLiteral("Type package id %1 before rehydrating.").arg(expected));
        postStatusMessage(rehydrateStatusLabel_->text(), StatusToast::Severity::Warn);
        return;
    }
    const auto targetNamespace = rehydrateNamespaceEdit_->text().trimmed();
    if (targetNamespace.isEmpty()) {
        rehydrateStatusLabel_->setText(QStringLiteral("Target namespace is required."));
        postStatusMessage(rehydrateStatusLabel_->text(), StatusToast::Severity::Warn);
        return;
    }

    rehydrateExecuteInFlight_ = true;
    setRehydrateBusy(true);
    resetRehydrateProgress();
    savorqt::db::ArchiveRehydrateExecuteRequest request{};
    request.archive_package_id = selected->archive_package_id;
    request.target_namespace = targetNamespace.toStdString();
    request.trace_id = "qt-archive-workbench-rehydrate";
    QPointer<ArchiveWorkbenchPage> self(this);
    request.progress_sink = [self](const savor::db::archive::ArchiveOperationProgress& progress) {
        if (self.isNull()) {
            return;
        }
        QMetaObject::invokeMethod(
            self.data(),
            [self, progress]() {
                if (!self.isNull()) {
                    self->applyRehydrateProgress(progress);
                }
            },
            Qt::QueuedConnection);
    };
    rehydrateExecuteWatcher_.setFuture(QtConcurrent::run([request]() {
        return savorqt::db::SavorDbArchiveService::ExecuteRehydrate(request);
    }));
}

void ArchiveWorkbenchPage::cleanupSelectedRehydrateRequest()
{
    const auto selected = selectedRehydrateRequest();
    if (!selected.has_value() || !isInactiveRehydrateStatus(selected->status)) return;
    rehydrateCleanupInFlight_ = true;
    setRehydrateBusy(true);
    const auto requestId = selected->rehydrate_request_id;
    rehydrateCleanupWatcher_.setFuture(QtConcurrent::run([requestId]() {
        return savorqt::db::SavorDbArchiveService::CleanupRehydrateRequest(requestId);
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

void ArchiveWorkbenchPage::applyArchivePackageRows(const std::vector<savor::db::UiArchiveCatalogRow>& rows)
{
    currentArchivePackages_ = rows;
    bool stillSelected = false;
    QSignalBlocker blocker(rehydratePackageTable_);
    rehydratePackageTable_->setRowCount(static_cast<int>(currentArchivePackages_.size()));
    for (int r = 0; r < static_cast<int>(currentArchivePackages_.size()); ++r) {
        const auto& row = currentArchivePackages_[static_cast<std::size_t>(r)];
        auto* idItem = makeItem(QString::number(static_cast<qint64>(row.archive_package_id)));
        idItem->setData(kArchivePackageIdRole, static_cast<qint64>(row.archive_package_id));
        rehydratePackageTable_->setItem(r, 0, idItem);
        rehydratePackageTable_->setItem(r, 1, makeItem(packageDisplayName(row)));
        rehydratePackageTable_->setItem(r, 2, makeItem(QString::number(static_cast<qint64>(row.source_workflow_count))));
        rehydratePackageTable_->setItem(r, 3, makeItem(qstr(row.source_scope_kind)));
        rehydratePackageTable_->setItem(r, 4, makeItem(formatTime(row.created_at_utc)));
        rehydratePackageTable_->setItem(r, 5, makeItem(qstr(row.checksum_status)));
        QString lastRequest = QStringLiteral("-");
        for (const auto& request : currentRehydrateRequests_) {
            if (request.archive_package_id == row.archive_package_id) {
                lastRequest = qstr(request.status);
                break;
            }
        }
        rehydratePackageTable_->setItem(r, 6, makeItem(lastRequest));
        if (row.archive_package_id == selectedArchivePackageId_) {
            stillSelected = true;
            rehydratePackageTable_->selectRow(r);
        }
    }

    if (!stillSelected) {
        selectedArchivePackageId_ = currentArchivePackages_.empty() ? 0 : currentArchivePackages_.front().archive_package_id;
        if (!currentArchivePackages_.empty()) {
            rehydratePackageTable_->selectRow(0);
            rehydrateNamespaceEdit_->setText(fallbackRehydrateNamespace());
        } else {
            currentRehydratePreview_.reset();
            currentRehydrateRequests_.clear();
            applyRehydrateRequestRows({});
        }
    }
    rehydrateCatalogStatusLabel_->setText(QStringLiteral("Rows: %1").arg(currentArchivePackages_.size()));
}

void ArchiveWorkbenchPage::applyRehydrateRequestRows(const std::vector<savor::db::UiArchiveRehydrateRequestRow>& rows)
{
    currentRehydrateRequests_ = rows;
    QSignalBlocker blocker(rehydrateRequestTable_);
    rehydrateRequestTable_->setRowCount(static_cast<int>(currentRehydrateRequests_.size()));
    for (int r = 0; r < static_cast<int>(currentRehydrateRequests_.size()); ++r) {
        const auto& row = currentRehydrateRequests_[static_cast<std::size_t>(r)];
        auto* idItem = makeItem(QString::number(static_cast<qint64>(row.rehydrate_request_id)));
        idItem->setData(kRehydrateRequestIdRole, static_cast<qint64>(row.rehydrate_request_id));
        rehydrateRequestTable_->setItem(r, 0, idItem);
        rehydrateRequestTable_->setItem(r, 1, makeItem(qstr(row.status)));
        rehydrateRequestTable_->setItem(r, 2, makeItem(qstr(row.target_namespace)));
        rehydrateRequestTable_->setItem(r, 3, makeItem(formatTime(row.requested_at_utc)));
        rehydrateRequestTable_->setItem(r, 4, makeItem(qstr(row.error_text)));
    }
    if (!currentRehydrateRequests_.empty()) {
        rehydrateRequestTable_->selectRow(0);
    }
    for (int r = 0; r < rehydratePackageTable_->rowCount(); ++r) {
        auto* idItem = rehydratePackageTable_->item(r, 0);
        if (idItem == nullptr || idItem->data(kArchivePackageIdRole).toLongLong() != selectedArchivePackageId_) {
            continue;
        }
        const QString status = currentRehydrateRequests_.empty()
            ? QStringLiteral("-")
            : qstr(currentRehydrateRequests_.front().status);
        rehydratePackageTable_->setItem(r, 6, makeItem(status));
        break;
    }
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

void ArchiveWorkbenchPage::updateRehydratePreviewLabels()
{
    if (!rehydrateInlineMessage_.isEmpty()) {
        rehydrateStatusLabel_->setText(rehydrateInlineMessage_);
    }
    if (rehydratePackageFetchInFlight_) {
        rehydratePreviewLabel_->setText(QStringLiteral("Loading archive packages..."));
        rehydrateBlockersLabel_->clear();
        return;
    }
    const auto selected = selectedArchivePackage();
    if (!selected.has_value()) {
        rehydratePreviewLabel_->setText(QStringLiteral("Select an archive package."));
        rehydrateBlockersLabel_->clear();
        return;
    }
    if (rehydratePreviewFetchInFlight_) {
        rehydratePreviewLabel_->setText(QStringLiteral("Preview loading..."));
        return;
    }
    if (!currentRehydratePreview_.has_value()) {
        rehydratePreviewLabel_->setText(QStringLiteral("No rehydrate preview."));
        rehydrateBlockersLabel_->clear();
        return;
    }

    const auto& preview = currentRehydratePreview_->preview;
    rehydratePreviewLabel_->setText(QStringLiteral(
        "Package: %1\nName: %2\nManifest files: %3\nManifest rows: %4\nWorkflows: %5\nJobs: %6\nExecution rows: %7\nAnalysis rows: %8\nState SAV rows: %9\nSAV zip entries: %10")
        .arg(static_cast<qint64>(selected->archive_package_id))
        .arg(packageDisplayName(*selected))
        .arg(preview.manifest_file_count)
        .arg(preview.manifest_row_total)
        .arg(preview.expected_workflows)
        .arg(preview.expected_jobs)
        .arg(preview.execution_row_count)
        .arg(preview.analysis_row_count)
        .arg(preview.state_savestate_count)
        .arg(preview.savestate_zip_entry_count));
    rehydrateBlockersLabel_->setText(joinMessages(preview.blocking_reasons));
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

void ArchiveWorkbenchPage::updateRehydrateExecuteState()
{
    const auto selected = selectedArchivePackage();
    const bool hasPreview = currentRehydratePreview_.has_value();
    const bool previewOk = hasPreview && currentRehydratePreview_->preview.success;
    const QString expected = selected.has_value()
        ? QString::number(static_cast<qint64>(selected->archive_package_id))
        : QString();
    const bool confirmed = selected.has_value() && rehydrateConfirmationEdit_->text().trimmed() == expected;
    const bool hasNamespace = rehydrateNamespaceEdit_ != nullptr && !rehydrateNamespaceEdit_->text().trimmed().isEmpty();
    const bool busy = rehydrateExecuteInFlight_ || rehydrateCleanupInFlight_;
    if (rehydrateExecuteButton_ != nullptr) {
        rehydrateExecuteButton_->setEnabled(!busy && !rehydratePreviewFetchInFlight_ && previewOk && confirmed && hasNamespace);
    }
    const auto request = selectedRehydrateRequest();
    if (rehydrateCleanupButton_ != nullptr) {
        rehydrateCleanupButton_->setEnabled(!busy && request.has_value() && isInactiveRehydrateStatus(request->status));
    }

    if (rehydrateStatusLabel_ == nullptr || !rehydrateInlineMessage_.isEmpty()) {
        return;
    }
    if (busy) {
        rehydrateStatusLabel_->setText(rehydrateExecuteInFlight_ ? QStringLiteral("Rehydrate operation running...") : QStringLiteral("Cleanup running..."));
    } else if (!selected.has_value()) {
        rehydrateStatusLabel_->setText(QStringLiteral("Select an archive package."));
    } else if (rehydratePreviewFetchInFlight_) {
        rehydrateStatusLabel_->setText(QStringLiteral("Waiting for rehydrate preview..."));
    } else if (!previewOk) {
        rehydrateStatusLabel_->setText(QStringLiteral("Resolve preview blockers before rehydrating."));
    } else if (!confirmed) {
        rehydrateStatusLabel_->setText(QStringLiteral("Type %1 to confirm package restore.").arg(expected));
    } else {
        rehydrateStatusLabel_->setText(QStringLiteral("Ready to rehydrate package."));
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

void ArchiveWorkbenchPage::setRehydrateBusy(bool busy)
{
    for (auto* widget : { rehydrateRefreshButton_, rehydrateExecuteButton_, rehydrateCleanupButton_ }) {
        if (widget != nullptr) widget->setEnabled(!busy);
    }
    if (rehydratePackageTable_ != nullptr) rehydratePackageTable_->setEnabled(!busy);
    if (rehydrateRequestTable_ != nullptr) rehydrateRequestTable_->setEnabled(!busy);
    if (rehydrateNamespaceEdit_ != nullptr) rehydrateNamespaceEdit_->setEnabled(!busy);
    if (rehydrateConfirmationEdit_ != nullptr) rehydrateConfirmationEdit_->setEnabled(!busy);
    updateRehydrateExecuteState();
}

void ArchiveWorkbenchPage::resetArchiveProgress()
{
    archiveProgressPhaseLabel_->setText(QStringLiteral("Starting archive operation..."));
    archiveProgressBar_->setRange(0, 0);
    archiveProgressBar_->setValue(0);
}

void ArchiveWorkbenchPage::resetRehydrateProgress()
{
    rehydrateProgressPhaseLabel_->setText(QStringLiteral("Starting rehydrate operation..."));
    rehydrateProgressBar_->setRange(0, 0);
    rehydrateProgressBar_->setValue(0);
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

void ArchiveWorkbenchPage::applyRehydrateProgress(const savor::db::archive::ArchiveOperationProgress& progress)
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
    rehydrateProgressPhaseLabel_->setText(text);

    if (progress.phase == savor::db::archive::ArchiveOperationPhase::Failed) {
        rehydrateProgressBar_->setRange(0, 1);
        rehydrateProgressBar_->setValue(0);
        return;
    }
    if (progress.indeterminate || progress.total_units <= 0) {
        rehydrateProgressBar_->setRange(0, 0);
        return;
    }
    rehydrateProgressBar_->setRange(0, static_cast<int>(std::min<std::int64_t>(progress.total_units, std::numeric_limits<int>::max())));
    rehydrateProgressBar_->setValue(static_cast<int>(std::clamp<std::int64_t>(progress.completed_units, 0, rehydrateProgressBar_->maximum())));
}

QString ArchiveWorkbenchPage::fallbackArchiveName() const
{
    const auto timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    return qstr(savorqt::db::SavorDbArchiveService::BuildFallbackArchiveName(
        currentFilter(),
        currentPreview_.has_value() ? currentPreview_->selection.selected_count : 0,
        timestamp.toStdString()));
}

QString ArchiveWorkbenchPage::fallbackRehydrateNamespace() const
{
    const auto id = selectedArchivePackageId_ > 0 ? selectedArchivePackageId_ : 0;
    return QStringLiteral("rehydrate_%1_%2")
        .arg(static_cast<qint64>(id))
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss")));
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

savorqt::db::ArchivePackageFilter ArchiveWorkbenchPage::currentArchivePackageFilter() const
{
    savorqt::db::ArchivePackageFilter filter{};
    filter.text_filter = rehydrateSearchEdit_->text().trimmed().toStdString();
    filter.source_scope_kind = rehydrateScopeFilter_->currentData().toString().trimmed().toStdString();
    filter.checksum_status = rehydrateChecksumFilter_->currentData().toString().trimmed().toStdString();
    filter.workflow_packages_only = rehydrateWorkflowOnlyCheck_->isChecked();
    if (rehydrateCreatedFromEnabled_->isChecked()) {
        filter.created_from_utc = rehydrateCreatedFromEdit_->dateTime().toMSecsSinceEpoch();
    }
    if (rehydrateCreatedToEnabled_->isChecked()) {
        filter.created_to_utc = rehydrateCreatedToEdit_->dateTime().toMSecsSinceEpoch();
    }
    return filter;
}

std::optional<savor::db::UiArchiveCatalogRow> ArchiveWorkbenchPage::selectedArchivePackage() const
{
    if (selectedArchivePackageId_ <= 0) {
        return std::nullopt;
    }
    const auto it = std::find_if(
        currentArchivePackages_.begin(),
        currentArchivePackages_.end(),
        [this](const savor::db::UiArchiveCatalogRow& row) {
            return row.archive_package_id == selectedArchivePackageId_;
        });
    if (it == currentArchivePackages_.end()) {
        return std::nullopt;
    }
    return *it;
}

std::optional<savor::db::UiArchiveRehydrateRequestRow> ArchiveWorkbenchPage::selectedRehydrateRequest() const
{
    if (rehydrateRequestTable_ == nullptr) {
        return std::nullopt;
    }
    const auto selectedItems = rehydrateRequestTable_->selectedItems();
    if (selectedItems.empty()) {
        return std::nullopt;
    }
    const int row = selectedItems.front()->row();
    if (row < 0 || row >= static_cast<int>(currentRehydrateRequests_.size())) {
        return std::nullopt;
    }
    return currentRehydrateRequests_[static_cast<std::size_t>(row)];
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
