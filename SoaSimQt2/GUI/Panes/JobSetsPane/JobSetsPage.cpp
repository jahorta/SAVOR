#include "JobSetsPage.h"

#include "JobSetsController.h"
#include "JobSetsProgressDelegate.h"
#include "JobSetsTreeModel.h"
#include "JobSetsTreeView.h"
#include "GUI/Widgets/ScrollBarStabilizer.h"
#include "DB/TagRepo.h"

#include <QtCore/QDateTime>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

JobSetsPage::JobSetsPage(QWidget* parent)
    : QWidget(parent)
    , controller_(new JobSetsController(this))
    , treeModel_(new JobSetsTreeModel(this))
    , progressDelegate_(std::make_unique<JobSetsProgressDelegate>(this))
{
    createWidgets();
    wireSignals();
    treeView_->attachModel(treeModel_);
    treeView_->setItemDelegateForColumn(JobSetsTreeModel::ProgressColumn, progressDelegate_.get());
}

void JobSetsPage::setPageActive(bool active)
{
    controller_->setPageActive(active);
}

void JobSetsPage::createWidgets()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(10);

    QFrame* toolbarPanel = new QFrame(this);
    toolbarPanel->setObjectName("jobSetsToolbarPanel");
    QGridLayout* toolbarLayout = new QGridLayout(toolbarPanel);
    toolbarLayout->setContentsMargins(8, 7, 8, 7);
    toolbarLayout->setHorizontalSpacing(10);
    toolbarLayout->setVerticalSpacing(10);

    kindFilter_ = new QComboBox(toolbarPanel);
    kindFilter_->setObjectName("jobSetsFilterCombo");
    stateFilter_ = new QComboBox(toolbarPanel);
    stateFilter_->setObjectName("jobSetsFilterCombo");
    tagFilter_ = new QComboBox(toolbarPanel);
    tagFilter_->setObjectName("jobSetsFilterCombo");
    pageSizeSpin_ = new QSpinBox(toolbarPanel);
    pageSizeSpin_->setObjectName("jobSetsSpin");
    pageSizeSpin_->setRange(10, 500);
    pageSizeSpin_->setSingleStep(10);
    applyButton_ = new QPushButton(QStringLiteral("Apply"), toolbarPanel);
    applyButton_->setObjectName("jobSetsPrimaryButton");
    resetButton_ = new QPushButton(QStringLiteral("Reset"), toolbarPanel);
    resetButton_->setObjectName("jobSetsSecondaryButton");
    autoRefreshCheck_ = new QCheckBox(QStringLiteral("Auto refresh"), toolbarPanel);
    autoRefreshCheck_->setObjectName("jobSetsCheckBox");
    refreshSecondsSpin_ = new QSpinBox(toolbarPanel);
    refreshSecondsSpin_->setObjectName("jobSetsSpin");
    refreshSecondsSpin_->setRange(1, 5);
    refreshSecondsSpin_->setSuffix(QStringLiteral(" s"));

    kindFilter_->addItem(QStringLiteral("All kinds"), QVariant());
    stateFilter_->addItem(QStringLiteral("All states"), QVariant());
    stateFilter_->addItem(QStringLiteral("Completed"), static_cast<int>(JobSetStateFilter::Completed));
    stateFilter_->addItem(QStringLiteral("Incomplete"), static_cast<int>(JobSetStateFilter::Incomplete));
    stateFilter_->addItem(QStringLiteral("Has failures"), static_cast<int>(JobSetStateFilter::HasFailures));
    tagFilter_->addItem(QStringLiteral("All tags"), QVariant());

    toolbarLayout->addWidget(new QLabel(QStringLiteral("Kind"), toolbarPanel), 0, 0);
    toolbarLayout->addWidget(kindFilter_, 1, 0);
    toolbarLayout->addWidget(new QLabel(QStringLiteral("State"), toolbarPanel), 0, 1);
    toolbarLayout->addWidget(stateFilter_, 1, 1);
    toolbarLayout->addWidget(new QLabel(QStringLiteral("Tag"), toolbarPanel), 0, 2);
    toolbarLayout->addWidget(tagFilter_, 1, 2);
    toolbarLayout->addWidget(new QLabel(QStringLiteral("Page size"), toolbarPanel), 0, 3);
    toolbarLayout->addWidget(pageSizeSpin_, 1, 3);
    toolbarLayout->addWidget(applyButton_, 1, 4);
    toolbarLayout->addWidget(resetButton_, 1, 5);
    toolbarLayout->addWidget(autoRefreshCheck_, 0, 6, 1, 2, Qt::AlignBottom);
    toolbarLayout->addWidget(refreshSecondsSpin_, 1, 6);
    toolbarLayout->addWidget(new QLabel(QStringLiteral("Interval"), toolbarPanel), 1, 7);
    toolbarLayout->setColumnStretch(0, 1);
    toolbarLayout->setColumnStretch(1, 1);

    rootLayout->addWidget(toolbarPanel);

    QFrame* contentPanel = new QFrame(this);
    contentPanel->setObjectName("jobSetsContentPanel");
    QVBoxLayout* contentLayout = new QVBoxLayout(contentPanel);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(10);

    QFrame* pagingPanel = new QFrame(contentPanel);
    pagingPanel->setObjectName("jobSetsPagingPanel");
    QHBoxLayout* pagingLayout = new QHBoxLayout(pagingPanel);
    pagingLayout->setContentsMargins(8, 7, 8, 7);
    pagingLayout->setSpacing(10);

    prevButton_ = new QPushButton(QStringLiteral("Prev"), pagingPanel);
    prevButton_->setObjectName("jobSetsSecondaryButton");
    nextButton_ = new QPushButton(QStringLiteral("Next"), pagingPanel);
    nextButton_->setObjectName("jobSetsSecondaryButton");
    refreshButton_ = new QPushButton(QStringLiteral("Refresh now"), pagingPanel);
    refreshButton_->setObjectName("jobSetsSecondaryButton");
    pageSummaryLabel_ = new QLabel(pagingPanel);
    pageSummaryLabel_->setObjectName("jobSetsMetaText");
    lastRefreshLabel_ = new QLabel(pagingPanel);
    lastRefreshLabel_->setObjectName("jobSetsMetaText");
    pageStatusLabel_ = new QLabel(pagingPanel);
    pageStatusLabel_->setObjectName("jobSetsMetaText");
    pageStatusLabel_->hide();

    pagingLayout->addWidget(prevButton_);
    pagingLayout->addWidget(nextButton_);
    pagingLayout->addWidget(refreshButton_);
    pagingLayout->addSpacing(8);
    pagingLayout->addWidget(pageSummaryLabel_);
    pagingLayout->addStretch();
    pagingLayout->addWidget(pageStatusLabel_);
    pagingLayout->addSpacing(10);
    pagingLayout->addWidget(lastRefreshLabel_);

    contentLayout->addWidget(pagingPanel);

    treeView_ = new JobSetsTreeView(contentPanel);
    contentLayout->addWidget(treeView_, 1);

    inlineMessageLabel_ = new QLabel(contentPanel);
    inlineMessageLabel_->setObjectName("jobSetsInlineMessage");
    inlineMessageLabel_->setWordWrap(true);
    contentLayout->addWidget(inlineMessageLabel_);

    loadingStateTimer_ = new QTimer(this);
    loadingStateTimer_->setSingleShot(true);

    rootLayout->addWidget(contentPanel, 1);
}

void JobSetsPage::wireSignals()
{
    connect(controller_, &JobSetsController::stateChanged, this, [this]() {
        syncControlsFromController();
        updateStatusWidgets();
    });
    connect(controller_, &JobSetsController::rowsChanged, this, &JobSetsPage::refreshModel);

    connect(applyButton_, &QPushButton::clicked, this, [this]() {
        controller_->applyFilters(selectedProgramKind(), selectedStateFilter(), selectedTagKey(), pageSizeSpin_->value());
    });
    connect(resetButton_, &QPushButton::clicked, controller_, &JobSetsController::resetFilters);
    connect(refreshButton_, &QPushButton::clicked, controller_, &JobSetsController::requestRefresh);
    connect(prevButton_, &QPushButton::clicked, controller_, &JobSetsController::requestPreviousPage);
    connect(nextButton_, &QPushButton::clicked, controller_, &JobSetsController::requestNextPage);
    connect(autoRefreshCheck_, &QCheckBox::toggled, controller_, &JobSetsController::setAutoRefreshEnabled);
    connect(refreshSecondsSpin_, qOverload<int>(&QSpinBox::valueChanged), controller_, &JobSetsController::setRefreshSeconds);

    connect(loadingStateTimer_, &QTimer::timeout, this, [this]() {
        if (controller_->viewState().loading && !controller_->viewState().familyItems.empty()) {
            delayedLoadingVisible_ = true;
            updateStatusWidgets();
        }
    });
}

void JobSetsPage::syncControlsFromController()
{
    const auto& state = controller_->viewState();

    {
        QSignalBlocker blocker(kindFilter_);
        const QVariant currentData = kindFilter_->currentData();
        kindFilter_->clear();
        kindFilter_->addItem(QStringLiteral("All kinds"), QVariant());
        QList<int> ids = state.programNames.keys();
        std::sort(ids.begin(), ids.end());
        for (int id : ids) {
            kindFilter_->addItem(state.programNames.value(id), id);
        }
        const int idx = kindFilter_->findData(currentData);
        kindFilter_->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    {
        QSignalBlocker blocker(pageSizeSpin_);
        pageSizeSpin_->setValue(state.pageLimit);
    }
    {
        QSignalBlocker blocker(tagFilter_);
        tagFilter_->clear();
        tagFilter_->addItem(QStringLiteral("All tags"), QVariant());
        const auto tagsResult = simcore::db::TagRepo::ListTagsForEntityKind("job_set");
        if (tagsResult.ok) {
            for (const auto& tag : tagsResult.value) {
                const QString key = QString::fromStdString(tag.tag_key);
                tagFilter_->addItem(key, key);
            }
        }
        const QVariant target = state.scope.tag_key.has_value() ? QVariant(QString::fromStdString(*state.scope.tag_key)) : QVariant();
        const int idx = target.isValid() ? tagFilter_->findData(target) : 0;
        tagFilter_->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    {
        QSignalBlocker blocker(autoRefreshCheck_);
        autoRefreshCheck_->setChecked(state.autoRefresh);
    }

    {
        QSignalBlocker blocker(refreshSecondsSpin_);
        refreshSecondsSpin_->setValue(state.refreshSeconds);
    }

    if (state.scope.program_kind.has_value()) {
        const int idx = kindFilter_->findData(*state.scope.program_kind);
        kindFilter_->setCurrentIndex(idx >= 0 ? idx : 0);
    } else {
        kindFilter_->setCurrentIndex(0);
    }

    if (state.scope.state_filter.has_value()) {
        const int idx = stateFilter_->findData(static_cast<int>(*state.scope.state_filter));
        stateFilter_->setCurrentIndex(idx >= 0 ? idx : 0);
    } else {
        stateFilter_->setCurrentIndex(0);
    }

    prevButton_->setEnabled(state.page.prev.has_value() && !state.actionsBusy);
    nextButton_->setEnabled(state.page.next.has_value() && !state.actionsBusy);
    refreshButton_->setEnabled(!state.actionsBusy);
    applyButton_->setEnabled(!state.actionsBusy);
    resetButton_->setEnabled(!state.actionsBusy);
    kindFilter_->setEnabled(!state.actionsBusy);
    stateFilter_->setEnabled(!state.actionsBusy);
    tagFilter_->setEnabled(!state.actionsBusy);
    pageSizeSpin_->setEnabled(!state.actionsBusy);
    autoRefreshCheck_->setEnabled(!state.actionsBusy);
    refreshSecondsSpin_->setEnabled(!state.actionsBusy);
}

void JobSetsPage::refreshModel()
{
    const auto& state = controller_->viewState();
    QSet<qint64> currentExpanded = treeView_->expandedJobSetIds();

    const ItemViewScrollSnapshot scrollSnapshot = captureItemViewScrollSnapshot(treeView_);

    programNames_ = state.programNames;
    treeModel_->syncRows(state.familyItems, state.programNames);

    QSet<qint64> pruned;
    for (qint64 id : currentExpanded) {
        if (treeModel_->containsJobSetId(id)) {
            pruned.insert(id);
        }
    }
    treeView_->restoreExpandedJobSetIds(pruned);
    restoreItemViewScrollSnapshot(treeView_, scrollSnapshot);
}

void JobSetsPage::updateStatusWidgets()
{
    const auto& state = controller_->viewState();
    updateLoadingIndicatorState();
    StatusToast::Severity toastSeverity = StatusToast::Severity::Info;
    QString toastMessage;

    pageSummaryLabel_->setText(QStringLiteral("Rows: %1 • page size: %2").arg(state.familyItems.size()).arg(state.pageLimit));
    lastRefreshLabel_->setText(state.lastRefresh.isValid()
        ? QStringLiteral("Last refresh: %1").arg(state.lastRefresh.toString(QStringLiteral("hh:mm:ss AP")))
        : QStringLiteral("Last refresh: --"));
    if (state.loading && (state.familyItems.empty() || delayedLoadingVisible_)) {
        pageStatusLabel_->setText(QStringLiteral("Loading job sets…"));
        pageStatusLabel_->show();
    } else {
        pageStatusLabel_->hide();
    }

    if (!state.errorMessage.isEmpty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("error"));
        inlineMessageLabel_->setText(state.errorMessage);
        inlineMessageLabel_->show();
        toastSeverity = StatusToast::Severity::Error;
        toastMessage = state.errorMessage;
    } else if (!state.infoMessage.isEmpty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
        inlineMessageLabel_->setText(state.infoMessage);
        inlineMessageLabel_->show();
        toastMessage = state.infoMessage;
    } else if (state.loading && state.familyItems.empty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
        inlineMessageLabel_->setText(QStringLiteral("Loading job sets…"));
        inlineMessageLabel_->show();
    } else if (state.familyItems.empty()) {
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
        inlineMessageLabel_->setText(QStringLiteral("No job sets matched the current filters."));
        inlineMessageLabel_->show();
    } else {
        inlineMessageLabel_->hide();
    }

    style()->unpolish(inlineMessageLabel_);
    style()->polish(inlineMessageLabel_);

    if (!toastMessage.isEmpty()) {
        const QString signature = QStringLiteral("%1|%2").arg(static_cast<int>(toastSeverity)).arg(toastMessage);
        if (signature != lastToastSignature_) {
            lastToastSignature_ = signature;
            emit statusToastRequested(StatusToast{ toastSeverity, toastMessage, QString(), 1, QDateTime{}, 4000 });
        }
    }
}

void JobSetsPage::updateLoadingIndicatorState()
{
    const auto& state = controller_->viewState();
    if (!state.loading) {
        delayedLoadingVisible_ = false;
        loadingStateTimer_->stop();
        return;
    }

    if (state.familyItems.empty()) {
        delayedLoadingVisible_ = true;
        loadingStateTimer_->stop();
        return;
    }

    if (delayedLoadingVisible_ || loadingStateTimer_->isActive()) {
        return;
    }

    loadingStateTimer_->start(1000);
}

std::optional<int> JobSetsPage::selectedProgramKind() const
{
    const QVariant data = kindFilter_->currentData();
    return data.isValid() ? std::optional<int>(data.toInt()) : std::nullopt;
}

std::optional<JobSetStateFilter> JobSetsPage::selectedStateFilter() const
{
    const QVariant data = stateFilter_->currentData();
    if (!data.isValid()) {
        return std::nullopt;
    }
    return static_cast<JobSetStateFilter>(data.toInt());
}

std::optional<QString> JobSetsPage::selectedTagKey() const
{
    const QVariant data = tagFilter_->currentData();
    return data.isValid() ? std::optional<QString>(data.toString()) : std::nullopt;
}
