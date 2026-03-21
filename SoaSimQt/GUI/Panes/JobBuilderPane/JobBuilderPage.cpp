#include "JobBuilderPage.h"

#include "GUI/Widgets/LedgerPickerDialog.h"
#include "Phases/DBPhaseBuilder/PhaseBuilderSchemas.h"
#include "Phases/DBPhaseBuilder/PhaseBuilderService.h"
#include "Phases/DBPhaseBuilder/PhaseBuilderPreview.h"
#include "Runner/IPC/Wire.h"
#include "DB/ExplorerSettingsPredicateRepo.h"

#include <QtCore/QDateTime>
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <exception>
#include <utility>

using simcore::PK_BattleSingleTurnRunner;
using simcore::PK_BattleTurnRunner;
using simcore::PK_SeedProbe;
using simcore::PK_TasMovie;
using simcore::db::DataService;
using simcore::db::DeltaSeedRepo;
using simcore::db::ProgramKindKV;
using simcore::db::phasebuilder::PhaseBuilderService;
using simcore::db::phasebuilder::PhasePreview;

namespace {
QString describeException(const char* prefix)
{
    try {
        throw;
    } catch (const std::exception& ex) {
        return QStringLiteral("%1: %2").arg(QString::fromUtf8(prefix), QString::fromUtf8(ex.what()));
    } catch (...) {
        return QStringLiteral("%1: unknown exception").arg(QString::fromUtf8(prefix));
    }
}

template <typename AsyncCall>
auto runAsync(AsyncCall&& asyncCall)
{
    return QtConcurrent::run([call = std::forward<AsyncCall>(asyncCall)]() mutable {
        return call();
    });
}

QFrame* createCard(const QString& title, QWidget* parent, QVBoxLayout** bodyLayout = nullptr)
{
    QFrame* frame = new QFrame(parent);
    frame->setObjectName("jobsSurfacePanel");
    QVBoxLayout* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    QLabel* titleLabel = new QLabel(title, frame);
    titleLabel->setObjectName("panelTitle");
    layout->addWidget(titleLabel);
    if (bodyLayout) {
        *bodyLayout = layout;
    }
    return frame;
}

QString yesNo(bool value)
{
    return value ? QStringLiteral("Yes") : QStringLiteral("No");
}
}

JobBuilderPage::JobBuilderPage(QWidget* parent)
    : QWidget(parent)
{
    createWidgets();
    wireSignals();
    loadProgramKinds();
}

void JobBuilderPage::createWidgets()
{
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(12);

    titleLabel_ = new QLabel(QStringLiteral("Job Builder"), this);
    titleLabel_->setObjectName("pageTitle");
    rootLayout->addWidget(titleLabel_);

    descriptionLabel_ = new QLabel(QStringLiteral("Qt-native builder for SeedProbe, TasMovie, and Explorer/BattleTurnRunner job sets with validation, preview, picker dialogs, and blueprint inspection."), this);
    descriptionLabel_->setObjectName("pageDescription");
    descriptionLabel_->setWordWrap(true);
    rootLayout->addWidget(descriptionLabel_);

    inlineMessageLabel_ = new QLabel(this);
    inlineMessageLabel_->setObjectName("jobSetsInlineMessage");
    inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
    inlineMessageLabel_->setWordWrap(true);
    inlineMessageLabel_->hide();
    rootLayout->addWidget(inlineMessageLabel_);

    QFrame* toolbar = new QFrame(this);
    toolbar->setObjectName("jobsToolbarPanel");
    QHBoxLayout* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(16, 14, 16, 14);
    toolbarLayout->setSpacing(10);

    kindCombo_ = new QComboBox(toolbar);
    kindCombo_->setMinimumWidth(220);
    resetDefaultsButton_ = new QPushButton(QStringLiteral("Reset to Defaults"), toolbar);
    validateButton_ = new QPushButton(QStringLiteral("Validate"), toolbar);
    previewButton_ = new QPushButton(QStringLiteral("Preview"), toolbar);
    resetDefaultsButton_->setObjectName("jobsSecondaryButton");
    validateButton_->setObjectName("jobsSecondaryButton");
    previewButton_->setObjectName("jobsSecondaryButton");

    toolbarLayout->addWidget(new QLabel(QStringLiteral("Program kind"), toolbar));
    toolbarLayout->addWidget(kindCombo_);
    toolbarLayout->addSpacing(8);
    toolbarLayout->addWidget(resetDefaultsButton_);
    toolbarLayout->addWidget(validateButton_);
    toolbarLayout->addWidget(previewButton_);
    toolbarLayout->addStretch();
    rootLayout->addWidget(toolbar);

    splitLayout_ = new QSplitter(Qt::Horizontal, this);
    splitLayout_->setChildrenCollapsible(false);

    QWidget* leftPanel = new QWidget(splitLayout_);
    QVBoxLayout* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(12);

    formStack_ = new QStackedWidget(leftPanel);
    leftLayout->addWidget(formStack_, 1);

    // SeedProbe form
    seedProbeForm_ = new QWidget(formStack_);
    QVBoxLayout* seedLayout = new QVBoxLayout(seedProbeForm_);
    seedLayout->setContentsMargins(0, 0, 0, 0);
    seedLayout->setSpacing(12);

    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("SeedProbe · General"), seedProbeForm_, &cardLayout);
        savestateSummaryLabel_ = new QLabel(QStringLiteral("No savestate selected."), card);
        savestateSummaryLabel_->setWordWrap(true);
        pickSavestateButton_ = new QPushButton(QStringLiteral("Pick Savestate…"), card);
        pickSavestateButton_->setObjectName("jobsSecondaryButton");
        seedProbePrioritySpin_ = new QSpinBox(card);
        seedProbePrioritySpin_->setRange(-100000, 100000);
        seedProbeRunMsSpin_ = new QSpinBox(card);
        seedProbeRunMsSpin_->setRange(0, 1000000000);
        seedProbeViMsSpin_ = new QSpinBox(card);
        seedProbeViMsSpin_->setRange(0, 1000000000);
        seedProbeClearWinnersCheck_ = new QCheckBox(QStringLiteral("Clear result winners"), card);
        QFormLayout* form = new QFormLayout();
        form->addRow(QStringLiteral("Savestate"), savestateSummaryLabel_);
        form->addRow(QString(), pickSavestateButton_);
        form->addRow(QStringLiteral("Priority"), seedProbePrioritySpin_);
        form->addRow(QStringLiteral("Run ms"), seedProbeRunMsSpin_);
        form->addRow(QStringLiteral("VI stall ms"), seedProbeViMsSpin_);
        form->addRow(QString(), seedProbeClearWinnersCheck_);
        cardLayout->addLayout(form);
        seedLayout->addWidget(card);
    }
    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("SeedProbe · Grid"), seedProbeForm_, &cardLayout);
        seedProbeSamplesSpin_ = new QSpinBox(card);
        seedProbeSamplesSpin_->setRange(0, 100000);
        seedProbeMinValueSpin_ = new QSpinBox(card);
        seedProbeMinValueSpin_->setRange(0, 255);
        seedProbeMaxValueSpin_ = new QSpinBox(card);
        seedProbeMaxValueSpin_->setRange(0, 255);
        seedProbeCapTopCheck_ = new QCheckBox(QStringLiteral("Cap trigger top"), card);
        seedProbeIgnoreTriggerCheck_ = new QCheckBox(QStringLiteral("Ignore trigger min/max"), card);
        QFormLayout* form = new QFormLayout();
        form->addRow(QStringLiteral("Samples per axis"), seedProbeSamplesSpin_);
        form->addRow(QStringLiteral("Min value"), seedProbeMinValueSpin_);
        form->addRow(QStringLiteral("Max value"), seedProbeMaxValueSpin_);
        form->addRow(QString(), seedProbeCapTopCheck_);
        form->addRow(QString(), seedProbeIgnoreTriggerCheck_);
        cardLayout->addLayout(form);
        seedLayout->addWidget(card);
    }
    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("SeedProbe · Unique"), seedProbeForm_, &cardLayout);
        seedProbeComboAttemptsSpin_ = new QSpinBox(card);
        seedProbeComboAttemptsSpin_->setRange(0, 1000000);
        seedProbeComboSamplerTriesSpin_ = new QSpinBox(card);
        seedProbeComboSamplerTriesSpin_->setRange(0, 1000000);
        QFormLayout* form = new QFormLayout();
        form->addRow(QStringLiteral("Combo attempts / target"), seedProbeComboAttemptsSpin_);
        form->addRow(QStringLiteral("Combo sampler tries"), seedProbeComboSamplerTriesSpin_);
        cardLayout->addLayout(form);
        seedLayout->addWidget(card);
    }
    seedLayout->addStretch();
    formStack_->addWidget(seedProbeForm_);

    // Tas form
    tasMovieForm_ = new QWidget(formStack_);
    QVBoxLayout* tasLayout = new QVBoxLayout(tasMovieForm_);
    tasLayout->setContentsMargins(0, 0, 0, 0);
    tasLayout->setSpacing(12);
    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("TasMovie"), tasMovieForm_, &cardLayout);
        artifactSummaryLabel_ = new QLabel(QStringLiteral("No .dtm artifact selected."), card);
        artifactSummaryLabel_->setWordWrap(true);
        pickArtifactButton_ = new QPushButton(QStringLiteral("Pick .dtm Artifact…"), card);
        pickArtifactButton_->setObjectName("jobsSecondaryButton");
        tasRtcLowEdit_ = new QLineEdit(card);
        tasRtcHighEdit_ = new QLineEdit(card);
        tasPrioritySpin_ = new QSpinBox(card);
        tasPrioritySpin_->setRange(-100000, 100000);
        tasRunMsSpin_ = new QSpinBox(card);
        tasRunMsSpin_->setRange(0, 1000000000);
        tasViMsSpin_ = new QSpinBox(card);
        tasViMsSpin_->setRange(0, 1000000000);
        tasHeadroomSpin_ = new QSpinBox(card);
        tasHeadroomSpin_->setRange(0, 1000000000);
        tasProgressEnableCheck_ = new QCheckBox(QStringLiteral("Enable progress"), card);
        tasAutoQueueCheck_ = new QCheckBox(QStringLiteral("Auto queue seeds"), card);
        QFormLayout* form = new QFormLayout();
        form->addRow(QStringLiteral("Artifact"), artifactSummaryLabel_);
        form->addRow(QString(), pickArtifactButton_);
        form->addRow(QStringLiteral("RTC low"), tasRtcLowEdit_);
        form->addRow(QStringLiteral("RTC high"), tasRtcHighEdit_);
        form->addRow(QStringLiteral("Priority"), tasPrioritySpin_);
        form->addRow(QStringLiteral("Run ms"), tasRunMsSpin_);
        form->addRow(QStringLiteral("VI stall ms"), tasViMsSpin_);
        form->addRow(QStringLiteral("Headroom x10"), tasHeadroomSpin_);
        form->addRow(QString(), tasProgressEnableCheck_);
        form->addRow(QString(), tasAutoQueueCheck_);
        cardLayout->addLayout(form);
        tasLayout->addWidget(card);
    }
    tasLayout->addStretch();
    formStack_->addWidget(tasMovieForm_);

    // Explorer form
    explorerForm_ = new QWidget(formStack_);
    QVBoxLayout* explorerLayout = new QVBoxLayout(explorerForm_);
    explorerLayout->setContentsMargins(0, 0, 0, 0);
    explorerLayout->setSpacing(12);
    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("ExplorerRun · Selected Settings and SeedProbes"), explorerForm_, &cardLayout);
        QGridLayout* grid = new QGridLayout();
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(10);
        explorerSettingsList_ = new QListWidget(card);
        explorerSeedProbesList_ = new QListWidget(card);
        addExplorerSettingsButton_ = new QPushButton(QStringLiteral("Add ExplorerSettings…"), card);
        clearExplorerSettingsButton_ = new QPushButton(QStringLiteral("Clear Settings"), card);
        addExplorerSeedProbeButton_ = new QPushButton(QStringLiteral("Add SeedProbe…"), card);
        clearExplorerSeedProbeButton_ = new QPushButton(QStringLiteral("Clear SeedProbes"), card);
        addExplorerSettingsButton_->setObjectName("jobsSecondaryButton");
        clearExplorerSettingsButton_->setObjectName("jobsSecondaryButton");
        addExplorerSeedProbeButton_->setObjectName("jobsSecondaryButton");
        clearExplorerSeedProbeButton_->setObjectName("jobsSecondaryButton");
        grid->addWidget(new QLabel(QStringLiteral("Explorer settings"), card), 0, 0);
        grid->addWidget(new QLabel(QStringLiteral("Seed probes"), card), 0, 1);
        grid->addWidget(explorerSettingsList_, 1, 0);
        grid->addWidget(explorerSeedProbesList_, 1, 1);
        QHBoxLayout* settingsButtons = new QHBoxLayout();
        settingsButtons->addWidget(addExplorerSettingsButton_);
        settingsButtons->addWidget(clearExplorerSettingsButton_);
        QHBoxLayout* seedButtons = new QHBoxLayout();
        seedButtons->addWidget(addExplorerSeedProbeButton_);
        seedButtons->addWidget(clearExplorerSeedProbeButton_);
        grid->addLayout(settingsButtons, 2, 0);
        grid->addLayout(seedButtons, 2, 1);
        cardLayout->addLayout(grid);
        explorerLayout->addWidget(card);
    }
    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("ExplorerRun · Delta Seed Selection"), explorerForm_, &cardLayout);
        QLabel* helper = new QLabel(QStringLiteral("Qt-native delta selection: each seed probe appears as a parent node. Check zero child rows to keep the default \"all deltas\" behavior, or check specific delta IDs to pin that probe to explicit deltas."), card);
        helper->setWordWrap(true);
        cardLayout->addWidget(helper);
        QHBoxLayout* buttons = new QHBoxLayout();
        useAllDeltasButton_ = new QPushButton(QStringLiteral("Use All for Selected Probe"), card);
        selectAllDeltasButton_ = new QPushButton(QStringLiteral("Select All for Selected Probe"), card);
        useAllDeltasButton_->setObjectName("jobsSecondaryButton");
        selectAllDeltasButton_->setObjectName("jobsSecondaryButton");
        buttons->addWidget(useAllDeltasButton_);
        buttons->addWidget(selectAllDeltasButton_);
        buttons->addStretch();
        cardLayout->addLayout(buttons);
        deltaTree_ = new QTreeWidget(card);
        deltaTree_->setColumnCount(2);
        deltaTree_->setHeaderLabels(QStringList{ QStringLiteral("SeedProbe / Delta"), QStringLiteral("Status") });
        cardLayout->addWidget(deltaTree_);
        explorerLayout->addWidget(card);
    }
    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("ExplorerRun · Options"), explorerForm_, &cardLayout);
        explorerPrioritySpin_ = new QSpinBox(card);
        explorerPrioritySpin_->setRange(-100000, 100000);
        explorerRunMsSpin_ = new QSpinBox(card);
        explorerRunMsSpin_->setRange(0, 1000000000);
        explorerViMsSpin_ = new QSpinBox(card);
        explorerViMsSpin_->setRange(0, 1000000000);
        explorerProgressEnableCheck_ = new QCheckBox(QStringLiteral("Enable progress"), card);
        explorerSingleTurnCheck_ = new QCheckBox(QStringLiteral("Use single-turn runner"), card);
        explorerAutoWaveTriggerCheck_ = new QCheckBox(QStringLiteral("Auto wave trigger enable"), card);
        explorerMinFakeAttacksSpin_ = new QSpinBox(card);
        explorerMinFakeAttacksSpin_->setRange(0, 1000000000);
        explorerMaxFakeAttacksSpin_ = new QSpinBox(card);
        explorerMaxFakeAttacksSpin_->setRange(0, 1000000000);
        QFormLayout* form = new QFormLayout();
        form->addRow(QStringLiteral("Priority"), explorerPrioritySpin_);
        form->addRow(QStringLiteral("Run ms"), explorerRunMsSpin_);
        form->addRow(QStringLiteral("VI stall ms"), explorerViMsSpin_);
        form->addRow(QString(), explorerProgressEnableCheck_);
        form->addRow(QString(), explorerSingleTurnCheck_);
        form->addRow(QString(), explorerAutoWaveTriggerCheck_);
        form->addRow(QStringLiteral("Min fake attacks"), explorerMinFakeAttacksSpin_);
        form->addRow(QStringLiteral("Max fake attacks"), explorerMaxFakeAttacksSpin_);
        cardLayout->addLayout(form);
        explorerLayout->addWidget(card);
    }
    explorerLayout->addStretch();
    formStack_->addWidget(explorerForm_);

    QWidget* rightPanel = new QWidget(splitLayout_);
    QVBoxLayout* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(12);

    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("Validation"), rightPanel, &cardLayout);
        validationText_ = new QPlainTextEdit(card);
        validationText_->setReadOnly(true);
        validationText_->setMinimumHeight(140);
        cardLayout->addWidget(validationText_);
        rightLayout->addWidget(card);
    }
    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("Preview"), rightPanel, &cardLayout);
        previewText_ = new QPlainTextEdit(card);
        previewText_->setReadOnly(true);
        previewText_->setMinimumHeight(180);
        cardLayout->addWidget(previewText_);
        rightLayout->addWidget(card);
    }
    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("Submit"), rightPanel, &cardLayout);
        submitStatusLabel_ = new QLabel(card);
        submitStatusLabel_->setWordWrap(true);
        purposeEdit_ = new QLineEdit(card);
        metaEdit_ = new QPlainTextEdit(card);
        metaEdit_->setMinimumHeight(100);
        submitButton_ = new QPushButton(QStringLiteral("Create Job Set & Enqueue"), card);
        submitButton_->setObjectName("jobsPrimaryButton");
        QFormLayout* form = new QFormLayout();
        form->addRow(QStringLiteral("Status"), submitStatusLabel_);
        form->addRow(QStringLiteral("Purpose"), purposeEdit_);
        form->addRow(QStringLiteral("Meta"), metaEdit_);
        cardLayout->addLayout(form);
        cardLayout->addWidget(submitButton_);
        rightLayout->addWidget(card);
    }
    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("Blueprint (INI)"), rightPanel, &cardLayout);
        iniText_ = new QPlainTextEdit(card);
        iniText_->setReadOnly(true);
        iniText_->setMinimumHeight(220);
        cardLayout->addWidget(iniText_);
        rightLayout->addWidget(card, 1);
    }

    splitLayout_->addWidget(leftPanel);
    splitLayout_->addWidget(rightPanel);
    splitLayout_->setStretchFactor(0, 3);
    splitLayout_->setStretchFactor(1, 2);
    rootLayout->addWidget(splitLayout_, 1);
}

void JobBuilderPage::wireSignals()
{
    connect(&kindsWatcher_, &QFutureWatcher<simcore::db::DbResult<std::vector<ProgramKindKV>>>::finished, this, [this]() {
        try {
            const auto result = kindsWatcher_.result();
            if (result.ok) {
                programKinds_.clear();
                for (const ProgramKindKV& kind : result.value) {
                    if (kind.name == "BattleSingleTurnRunner") {
                        continue;
                    }
                    programKinds_.push_back(kind);
                }
                kindCombo_->clear();
                int seedProbeIndex = -1;
                for (const ProgramKindKV& kind : programKinds_) {
                    kindCombo_->addItem(QString::fromStdString(kind.name), kind.id);
                    if (kind.id == PK_SeedProbe) {
                        seedProbeIndex = kindCombo_->count() - 1;
                    }
                }
                if (seedProbeIndex >= 0) {
                    kindCombo_->setCurrentIndex(seedProbeIndex);
                } else if (kindCombo_->count() > 0) {
                    kindCombo_->setCurrentIndex(0);
                }
                handleKindSelectionChanged();
            } else {
                setInlineMessage(QStringLiteral("Failed to load program kinds: %1").arg(QString::fromStdString(result.error.message)), QStringLiteral("error"));
            }
        } catch (...) {
            setInlineMessage(describeException("Failed to load program kinds"), QStringLiteral("error"));
        }
    });

    connect(&previewWatcher_, &QFutureWatcher<simcore::db::DbResult<PhasePreview>>::finished, this, [this]() {
        previewBusy_ = false;
        try {
            const auto result = previewWatcher_.result();
            if (result.ok) {
                preview_ = result.value;
                previewErrorMessage_.clear();
            } else {
                preview_.reset();
                previewErrorMessage_ = QString::fromStdString(result.error.message);
            }
        } catch (...) {
            preview_.reset();
            previewErrorMessage_ = describeException("Preview failed");
        }
        refreshPreviewPanel();
        refreshSubmitPanel();
        updateStatusMessage();
    });

    connect(&submitWatcher_, &QFutureWatcher<SubmitResultPayload>::finished, this, [this]() {
        submitBusy_ = false;
        try {
            const SubmitResultPayload result = submitWatcher_.result();
            if (result.ok) {
                submitErrorMessage_.clear();
                submitInfoMessage_ = QStringLiteral("Created %1 job set(s). First Job Set ID: %2.")
                    .arg(result.createdCount)
                    .arg(result.firstJobSetId);
            } else {
                submitInfoMessage_.clear();
                submitErrorMessage_ = result.errorMessage;
            }
        } catch (...) {
            submitInfoMessage_.clear();
            submitErrorMessage_ = describeException("Submit failed");
        }
        refreshSubmitPanel();
        updateStatusMessage();
    });

    connect(kindCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { handleKindSelectionChanged(); });
    connect(resetDefaultsButton_, &QPushButton::clicked, this, [this]() { loadDefaultsForSelectedKind(); });
    connect(validateButton_, &QPushButton::clicked, this, [this]() { syncIniFromWidgets(); refreshValidation(); refreshSubmitPanel(); updateStatusMessage(); });
    connect(previewButton_, &QPushButton::clicked, this, [this]() { requestPreview(); });
    connect(submitButton_, &QPushButton::clicked, this, [this]() { requestSubmit(); });

    connect(pickSavestateButton_, &QPushButton::clicked, this, &JobBuilderPage::openSavestatePicker);
    connect(pickArtifactButton_, &QPushButton::clicked, this, &JobBuilderPage::openArtifactPicker);
    connect(addExplorerSeedProbeButton_, &QPushButton::clicked, this, &JobBuilderPage::openSeedProbePicker);
    connect(addExplorerSettingsButton_, &QPushButton::clicked, this, &JobBuilderPage::openExplorerSettingsPicker);
    connect(clearExplorerSeedProbeButton_, &QPushButton::clicked, this, [this]() {
        selectedSeedProbeIds_.clear();
        deltaRowsByProbe_.clear();
        selectedDeltaIdsByProbe_.clear();
        primarySeedProbeId_ = 0;
        syncIniFromWidgets();
    });
    connect(clearExplorerSettingsButton_, &QPushButton::clicked, this, [this]() {
        selectedExplorerSettingsIds_.clear();
        primaryExplorerSettingsId_ = 0;
        syncIniFromWidgets();
    });
    connect(useAllDeltasButton_, &QPushButton::clicked, this, [this]() {
        QTreeWidgetItem* item = deltaTree_->currentItem();
        if (!item) {
            return;
        }
        QTreeWidgetItem* parent = item->parent() ? item->parent() : item;
        const qint64 probeId = parent->data(0, Qt::UserRole).toLongLong();
        selectedDeltaIdsByProbe_.remove(probeId);
        refreshDeltaTree();
        syncIniFromWidgets();
    });
    connect(selectAllDeltasButton_, &QPushButton::clicked, this, [this]() {
        QTreeWidgetItem* item = deltaTree_->currentItem();
        if (!item) {
            return;
        }
        QTreeWidgetItem* parent = item->parent() ? item->parent() : item;
        const qint64 probeId = parent->data(0, Qt::UserRole).toLongLong();
        if (!deltaRowsByProbe_.contains(probeId)) {
            return;
        }
        QSet<qint64> selectedIds;
        for (const auto& row : deltaRowsByProbe_.value(probeId)) {
            selectedIds.insert(row.id);
        }
        selectedDeltaIdsByProbe_.insert(probeId, selectedIds);
        refreshDeltaTree();
        syncIniFromWidgets();
    });
    connect(deltaTree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (!item || column != 0 || !item->parent()) {
            return;
        }
        const qint64 probeId = item->parent()->data(0, Qt::UserRole).toLongLong();
        const qint64 deltaId = item->data(0, Qt::UserRole).toLongLong();
        QSet<qint64> selected = selectedDeltaIdsByProbe_.value(probeId);
        if (item->checkState(0) == Qt::Checked) {
            selected.insert(deltaId);
        } else {
            selected.remove(deltaId);
        }
        if (selected.isEmpty()) {
            selectedDeltaIdsByProbe_.remove(probeId);
        } else {
            selectedDeltaIdsByProbe_.insert(probeId, selected);
        }
        syncIniFromWidgets();
    });

    const auto connectSpin = [this](QSpinBox* spin) {
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { syncIniFromWidgets(); });
    };
    const auto connectCheck = [this](QCheckBox* check) {
        connect(check, &QCheckBox::toggled, this, [this](bool) { syncIniFromWidgets(); });
    };
    connectSpin(seedProbePrioritySpin_);
    connectSpin(seedProbeRunMsSpin_);
    connectSpin(seedProbeViMsSpin_);
    connectSpin(seedProbeSamplesSpin_);
    connectSpin(seedProbeMinValueSpin_);
    connectSpin(seedProbeMaxValueSpin_);
    connectSpin(seedProbeComboAttemptsSpin_);
    connectSpin(seedProbeComboSamplerTriesSpin_);
    connectCheck(seedProbeClearWinnersCheck_);
    connectCheck(seedProbeCapTopCheck_);
    connectCheck(seedProbeIgnoreTriggerCheck_);

    connect(tasRtcLowEdit_, &QLineEdit::textChanged, this, [this](const QString&) { syncIniFromWidgets(); });
    connect(tasRtcHighEdit_, &QLineEdit::textChanged, this, [this](const QString&) { syncIniFromWidgets(); });
    connectSpin(tasPrioritySpin_);
    connectSpin(tasRunMsSpin_);
    connectSpin(tasViMsSpin_);
    connectSpin(tasHeadroomSpin_);
    connectCheck(tasProgressEnableCheck_);
    connectCheck(tasAutoQueueCheck_);

    connectSpin(explorerPrioritySpin_);
    connectSpin(explorerRunMsSpin_);
    connectSpin(explorerViMsSpin_);
    connectSpin(explorerMinFakeAttacksSpin_);
    connectSpin(explorerMaxFakeAttacksSpin_);
    connectCheck(explorerProgressEnableCheck_);
    connectCheck(explorerSingleTurnCheck_);
    connectCheck(explorerAutoWaveTriggerCheck_);

    connect(purposeEdit_, &QLineEdit::textChanged, this, [this](const QString&) { refreshSubmitPanel(); });
    connect(metaEdit_, &QPlainTextEdit::textChanged, this, [this]() { refreshSubmitPanel(); });
}

void JobBuilderPage::loadProgramKinds()
{
    kindsWatcher_.setFuture(runAsync([]() {
        return DataService::ListProgramKindsAsync().get();
    }));
}

void JobBuilderPage::handleKindSelectionChanged()
{
    const int programKind = selectedProgramKind();
    if (programKind == PK_TasMovie) {
        formStack_->setCurrentWidget(tasMovieForm_);
    } else if (programKind == PK_BattleTurnRunner) {
        formStack_->setCurrentWidget(explorerForm_);
    } else {
        formStack_->setCurrentWidget(seedProbeForm_);
    }
    loadDefaultsForSelectedKind();
}

void JobBuilderPage::loadDefaultsForSelectedKind()
{
    const int programKind = selectedProgramKind();
    if (programKind <= 0) {
        return;
    }

    ini_ = PhaseBuilderService::DefaultsFor(programKind);
    hasIni_ = true;
    preview_.reset();
    previewErrorMessage_.clear();
    submitErrorMessage_.clear();
    submitInfoMessage_.clear();
    selectedExplorerSettingsIds_.clear();
    selectedSeedProbeIds_.clear();
    deltaRowsByProbe_.clear();
    selectedDeltaIdsByProbe_.clear();
    savestateId_ = 0;
    artifactId_ = 0;
    primaryExplorerSettingsId_ = 0;
    primarySeedProbeId_ = 0;
    syncWidgetsFromIni();
    refreshSelectionLists();
    refreshDeltaTree();
    refreshValidation();
    refreshPreviewPanel();
    refreshSubmitPanel();
    refreshIniPanel();
    updateStatusMessage();
}

void JobBuilderPage::syncWidgetsFromIni()
{
    if (!hasIni_) {
        return;
    }

    if (selectedProgramKind() == PK_SeedProbe) {
        auto bp = simcore::db::codec::seedprobe::BlueprintIni::from_section(ini_);
        auto grid = simcore::db::codec::seedprobe::GridIni::from_section(ini_);
        auto uni = simcore::db::codec::seedprobe::UniqueIni::from_section(ini_);
        savestateId_ = bp.savestate_id;
        savestateSummaryLabel_->setText(savestateId_ > 0 ? QStringLiteral("Savestate ID %1").arg(savestateId_) : QStringLiteral("No savestate selected."));
        seedProbePrioritySpin_->setValue(bp.priority);
        seedProbeRunMsSpin_->setValue(static_cast<int>(bp.run_ms));
        seedProbeViMsSpin_->setValue(static_cast<int>(bp.vi_stall_ms));
        seedProbeClearWinnersCheck_->setChecked(bp.clear_result_winners);
        seedProbeSamplesSpin_->setValue(grid.samples_per_axis);
        seedProbeMinValueSpin_->setValue(grid.min_value);
        seedProbeMaxValueSpin_->setValue(grid.max_value);
        seedProbeCapTopCheck_->setChecked(grid.cap_trigger_top);
        seedProbeIgnoreTriggerCheck_->setChecked(grid.ignore_trigger_minmax);
        seedProbeComboAttemptsSpin_->setValue(uni.combo_attempts_per_target);
        seedProbeComboSamplerTriesSpin_->setValue(uni.combo_sampler_tries);
    } else if (selectedProgramKind() == PK_TasMovie) {
        auto bp = simcore::db::codec::tas::BlueprintIni::from_section(ini_);
        artifactId_ = bp.base_dtm_artifact_id;
        artifactSummaryLabel_->setText(artifactId_ > 0 ? QStringLiteral("Artifact ID %1").arg(artifactId_) : QStringLiteral("No .dtm artifact selected."));
        tasRtcLowEdit_->setText(QString::number(bp.rtc_low));
        tasRtcHighEdit_->setText(QString::number(bp.rtc_high));
        tasPrioritySpin_->setValue(bp.priority);
        tasRunMsSpin_->setValue(static_cast<int>(bp.run_ms));
        tasViMsSpin_->setValue(static_cast<int>(bp.vi_stall_ms));
        tasHeadroomSpin_->setValue(static_cast<int>(bp.headroom_x10));
        tasProgressEnableCheck_->setChecked(bp.progress_enable);
        tasAutoQueueCheck_->setChecked(bp.auto_queue_seeds);
    } else {
        auto bp = simcore::db::codec::battle::run::BlueprintIni::from_section(ini_);
        primaryExplorerSettingsId_ = bp.settings_id;
        primarySeedProbeId_ = bp.seed_probe_id;
        if (primaryExplorerSettingsId_ > 0) {
            selectedExplorerSettingsIds_.append(primaryExplorerSettingsId_);
        }
        if (primarySeedProbeId_ > 0) {
            selectedSeedProbeIds_.append(primarySeedProbeId_);
        }
        explorerPrioritySpin_->setValue(bp.priority);
        explorerRunMsSpin_->setValue(static_cast<int>(bp.run_ms));
        explorerViMsSpin_->setValue(static_cast<int>(bp.vi_stall_ms));
        explorerProgressEnableCheck_->setChecked(bp.progress_enable);
        explorerSingleTurnCheck_->setChecked(bp.use_single_turn_runner);
        explorerAutoWaveTriggerCheck_->setChecked(bp.auto_wave_trigger_enable);
        explorerMinFakeAttacksSpin_->setValue(static_cast<int>(bp.min_fake_attacks));
        explorerMaxFakeAttacksSpin_->setValue(static_cast<int>(bp.max_fake_attacks));
    }
}

void JobBuilderPage::syncIniFromWidgets()
{
    if (!hasIni_) {
        return;
    }

    if (selectedProgramKind() == PK_SeedProbe) {
        auto bp = simcore::db::codec::seedprobe::BlueprintIni::from_section(ini_);
        auto grid = simcore::db::codec::seedprobe::GridIni::from_section(ini_);
        auto uni = simcore::db::codec::seedprobe::UniqueIni::from_section(ini_);
        bp.savestate_id = savestateId_;
        bp.priority = seedProbePrioritySpin_->value();
        bp.run_ms = static_cast<uint32_t>(seedProbeRunMsSpin_->value());
        bp.vi_stall_ms = static_cast<uint32_t>(seedProbeViMsSpin_->value());
        bp.clear_result_winners = seedProbeClearWinnersCheck_->isChecked();
        bp.set_section(ini_);
        grid.samples_per_axis = seedProbeSamplesSpin_->value();
        grid.min_value = static_cast<uint8_t>(seedProbeMinValueSpin_->value());
        grid.max_value = static_cast<uint8_t>(seedProbeMaxValueSpin_->value());
        grid.cap_trigger_top = seedProbeCapTopCheck_->isChecked();
        grid.ignore_trigger_minmax = seedProbeIgnoreTriggerCheck_->isChecked();
        grid.set_section(ini_);
        uni.combo_attempts_per_target = seedProbeComboAttemptsSpin_->value();
        uni.combo_sampler_tries = seedProbeComboSamplerTriesSpin_->value();
        uni.set_section(ini_);
    } else if (selectedProgramKind() == PK_TasMovie) {
        auto bp = simcore::db::codec::tas::BlueprintIni::from_section(ini_);
        bp.base_dtm_artifact_id = artifactId_;
        bp.rtc_low = tasRtcLowEdit_->text().trimmed().toLongLong();
        bp.rtc_high = tasRtcHighEdit_->text().trimmed().toLongLong();
        bp.priority = tasPrioritySpin_->value();
        bp.run_ms = static_cast<uint32_t>(tasRunMsSpin_->value());
        bp.vi_stall_ms = static_cast<uint32_t>(tasViMsSpin_->value());
        bp.headroom_x10 = static_cast<uint32_t>(tasHeadroomSpin_->value());
        bp.progress_enable = tasProgressEnableCheck_->isChecked();
        bp.auto_queue_seeds = tasAutoQueueCheck_->isChecked();
        bp.set_section(ini_);
    } else {
        auto bp = simcore::db::codec::battle::run::BlueprintIni::from_section(ini_);
        if (!selectedExplorerSettingsIds_.isEmpty()) {
            primaryExplorerSettingsId_ = selectedExplorerSettingsIds_.front();
        }
        if (!selectedSeedProbeIds_.isEmpty()) {
            primarySeedProbeId_ = selectedSeedProbeIds_.front();
        }
        bp.settings_id = primaryExplorerSettingsId_;
        bp.seed_probe_id = primarySeedProbeId_;
        bp.delta_seed_ids_csv = "0";
        bp.priority = explorerPrioritySpin_->value();
        bp.run_ms = static_cast<uint32_t>(explorerRunMsSpin_->value());
        bp.vi_stall_ms = static_cast<uint32_t>(explorerViMsSpin_->value());
        bp.progress_enable = explorerProgressEnableCheck_->isChecked();
        bp.use_single_turn_runner = explorerSingleTurnCheck_->isChecked();
        bp.auto_wave_trigger_enable = explorerAutoWaveTriggerCheck_->isChecked();
        bp.min_fake_attacks = static_cast<uint32_t>(std::max(0, explorerMinFakeAttacksSpin_->value()));
        bp.max_fake_attacks = static_cast<uint32_t>(std::max(0, explorerMaxFakeAttacksSpin_->value()));
        bp.set_section(ini_);
    }

    refreshSelectionLists();
    refreshDeltaTree();
    refreshValidation();
    refreshSubmitPanel();
    refreshIniPanel();
    updateStatusMessage();
}

void JobBuilderPage::refreshValidation()
{
    validationEntries_.clear();
    if (!hasIni_) {
        validationText_->setPlainText(QStringLiteral("No blueprint loaded yet."));
        return;
    }

    const auto errors = PhaseBuilderService::Validate(selectedProgramKind(), ini_);
    for (const auto& entry : errors) {
        validationEntries_.append(ValidationEntry{ QString::fromStdString(entry.field), QString::fromStdString(entry.message) });
    }
    QVector<QString> lines;
    if (validationEntries_.isEmpty()) {
        lines.append(QStringLiteral("Validation passed. No issues detected."));
    } else {
        for (const auto& entry : validationEntries_) {
            lines.append(QStringLiteral("• %1: %2").arg(entry.field, entry.message));
        }
    }
    validationText_->setPlainText(joinLines(lines));
}

void JobBuilderPage::refreshPreviewPanel()
{
    QVector<QString> lines;
    if (previewBusy_) {
        lines.append(QStringLiteral("Previewing…"));
    } else if (!previewErrorMessage_.isEmpty()) {
        lines.append(QStringLiteral("Preview failed: %1").arg(previewErrorMessage_));
    } else if (preview_.has_value()) {
        const auto& preview = *preview_;
        if (preview.tasmovie.has_value()) {
            const auto& t = *preview.tasmovie;
            lines.append(QStringLiteral("TasMovie jobs: %1").arg(t.jobs));
            lines.append(QStringLiteral("RTC range: %1 → %2").arg(t.rtc_low).arg(t.rtc_high));
            lines.append(QStringLiteral("Artifact exists: %1").arg(yesNo(t.artifact_exists)));
            if (t.artifact_size.has_value()) {
                lines.append(QStringLiteral("Artifact size: %1 bytes").arg(*t.artifact_size));
            }
            for (const std::string& warning : t.warnings) {
                lines.append(QStringLiteral("• %1").arg(QString::fromStdString(warning)));
            }
        }
        if (preview.seedprobe.has_value()) {
            const auto& s = *preview.seedprobe;
            lines.append(QStringLiteral("Neutral jobs: %1").arg(s.neutral_jobs));
            lines.append(QStringLiteral("Grid jobs: %1").arg(s.grid_jobs));
            if (s.unique_jobs.has_value()) {
                lines.append(QStringLiteral("Unique jobs: %1").arg(*s.unique_jobs));
            } else if (s.unique_deferred) {
                lines.append(QStringLiteral("Unique jobs are deferred until grid deltas exist."));
            }
            for (const std::string& warning : s.warnings) {
                lines.append(QStringLiteral("• %1").arg(QString::fromStdString(warning)));
            }
        }
        if (preview.explorer.has_value()) {
            const auto& e = *preview.explorer;
            qint64 estimatedJobs = e.jobs;
            if (!selectedExplorerSettingsIds_.isEmpty() && !selectedSeedProbeIds_.isEmpty()) {
                estimatedJobs = static_cast<qint64>(selectedExplorerSettingsIds_.size()) * static_cast<qint64>(selectedSeedProbeIds_.size());
            }
            lines.append(QStringLiteral("Explorer jobs: %1").arg(estimatedJobs));
            lines.append(QStringLiteral("Predicates: %1").arg(e.predicate_count));
            for (const std::string& warning : e.warnings) {
                lines.append(QStringLiteral("• %1").arg(QString::fromStdString(warning)));
            }
        }
    } else {
        lines.append(QStringLiteral("No preview generated yet."));
    }
    previewText_->setPlainText(joinLines(lines));
}

void JobBuilderPage::refreshSubmitPanel()
{
    QString status;
    if (submitBusy_) {
        status = QStringLiteral("Submitting…");
    } else if (!submitErrorMessage_.isEmpty()) {
        status = submitErrorMessage_;
    } else if (!submitInfoMessage_.isEmpty()) {
        status = submitInfoMessage_;
    } else if (canSubmit()) {
        status = QStringLiteral("Validation is clean. Ready to create job set(s).");
    } else {
        status = QStringLiteral("Resolve validation errors before submitting.");
    }
    submitStatusLabel_->setText(status);
    submitButton_->setEnabled(canSubmit() && !submitBusy_);
}

void JobBuilderPage::refreshIniPanel()
{
    if (!hasIni_) {
        iniText_->setPlainText(QStringLiteral("No blueprint loaded yet."));
        return;
    }
    iniText_->setPlainText(QString::fromStdString(ini_.to_string_preserve_order()));
}

void JobBuilderPage::refreshSelectionLists()
{
    explorerSettingsList_->clear();
    for (qint64 id : selectedExplorerSettingsIds_) {
        explorerSettingsList_->addItem(QStringLiteral("ExplorerSettings %1%2").arg(id).arg(id == primaryExplorerSettingsId_ ? QStringLiteral(" (primary)") : QString()));
    }
    explorerSeedProbesList_->clear();
    for (qint64 id : selectedSeedProbeIds_) {
        explorerSeedProbesList_->addItem(QStringLiteral("SeedProbe %1%2").arg(id).arg(id == primarySeedProbeId_ ? QStringLiteral(" (primary)") : QString()));
    }
}

void JobBuilderPage::refreshDeltaTree()
{
    QSignalBlocker blocker(deltaTree_);
    deltaTree_->clear();
    for (qint64 probeId : selectedSeedProbeIds_) {
        QTreeWidgetItem* parent = new QTreeWidgetItem(deltaTree_);
        parent->setText(0, QStringLiteral("SeedProbe %1").arg(probeId));
        parent->setText(1, deltaRowsByProbe_.contains(probeId) ? QStringLiteral("%1 delta rows").arg(deltaRowsByProbe_.value(probeId).size()) : QStringLiteral("Loading…"));
        parent->setData(0, Qt::UserRole, probeId);
        if (!deltaRowsByProbe_.contains(probeId)) {
            continue;
        }
        const QSet<qint64> selectedIds = selectedDeltaIdsByProbe_.value(probeId);
        for (const auto& row : deltaRowsByProbe_.value(probeId)) {
            QTreeWidgetItem* child = new QTreeWidgetItem(parent);
            child->setText(0, QStringLiteral("delta %1 [id %2]").arg(row.seed_delta).arg(row.id));
            child->setText(1, selectedIds.contains(row.id) ? QStringLiteral("Selected") : QStringLiteral("Default all"));
            child->setData(0, Qt::UserRole, row.id);
            child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
            child->setCheckState(0, selectedIds.contains(row.id) ? Qt::Checked : Qt::Unchecked);
        }
        parent->setExpanded(true);
    }
}

void JobBuilderPage::updateStatusMessage()
{
    if (!submitErrorMessage_.isEmpty()) {
        setInlineMessage(submitErrorMessage_, QStringLiteral("error"));
    } else if (!previewErrorMessage_.isEmpty()) {
        setInlineMessage(previewErrorMessage_, QStringLiteral("error"));
    } else if (!submitInfoMessage_.isEmpty()) {
        setInlineMessage(submitInfoMessage_, QStringLiteral("info"));
    } else if (!validationEntries_.isEmpty()) {
        setInlineMessage(QStringLiteral("Validation has %1 issue(s). Preview and submit stay available only when validation is clean.").arg(validationEntries_.size()), QStringLiteral("info"));
    } else {
        inlineMessageLabel_->hide();
    }
}

void JobBuilderPage::setInlineMessage(const QString& text, const QString& severity)
{
    inlineMessageLabel_->setText(text);
    inlineMessageLabel_->setProperty("severity", severity);
    inlineMessageLabel_->style()->unpolish(inlineMessageLabel_);
    inlineMessageLabel_->style()->polish(inlineMessageLabel_);
    inlineMessageLabel_->show();
}

void JobBuilderPage::openSavestatePicker()
{
    LedgerPickerDialog<simcore::db::SavestateLite> dialog(
        QStringLiteral("Pick Savestate"),
        { { QStringLiteral("ID"), [](const simcore::db::SavestateLite& row) { return QString::number(row.id); } },
          { QStringLiteral("Type"), [](const simcore::db::SavestateLite& row) { return QString::number(row.savestate_type); } },
          { QStringLiteral("Note"), [](const simcore::db::SavestateLite& row) { return QString::fromStdString(row.note); } } },
        [](const PagedQuery<>& query, const QString& search) {
            return DataService::FetchSavestatesPage(query, search.toStdString()).get();
        },
        [](const simcore::db::SavestateLite& row) { return static_cast<qint64>(row.id); },
        [](const simcore::db::SavestateLite& row) {
            return QStringLiteral("Savestate %1 · type %2 · %3").arg(row.id).arg(row.savestate_type).arg(QString::fromStdString(row.note));
        },
        this);
    if (dialog.exec() == QDialog::Accepted && dialog.selectedRow().has_value()) {
        savestateId_ = dialog.selectedId();
        savestateSummaryLabel_->setText(QStringLiteral("Savestate ID %1").arg(savestateId_));
        syncIniFromWidgets();
    }
}

void JobBuilderPage::openArtifactPicker()
{
    LedgerPickerDialog<simcore::db::ObjectRefLite> dialog(
        QStringLiteral("Pick .dtm Artifact"),
        { { QStringLiteral("ID"), [](const simcore::db::ObjectRefLite& row) { return QString::number(row.id); } },
          { QStringLiteral("Filename"), [](const simcore::db::ObjectRefLite& row) { return QString::fromStdString(row.filename); } },
          { QStringLiteral("Size"), [](const simcore::db::ObjectRefLite& row) { return QString::number(row.size); } } },
        [](const PagedQuery<>& query, const QString& search) {
            return DataService::FetchObjectRefsPage(query, search.toStdString(), ".dtm").get();
        },
        [](const simcore::db::ObjectRefLite& row) { return static_cast<qint64>(row.id); },
        [](const simcore::db::ObjectRefLite& row) {
            return QStringLiteral("Artifact %1 · %2").arg(row.id).arg(QString::fromStdString(row.filename));
        },
        this);
    if (dialog.exec() == QDialog::Accepted && dialog.selectedRow().has_value()) {
        artifactId_ = dialog.selectedId();
        artifactSummaryLabel_->setText(QStringLiteral("Artifact ID %1").arg(artifactId_));
        syncIniFromWidgets();
    }
}

void JobBuilderPage::openSeedProbePicker()
{
    LedgerPickerDialog<simcore::db::SeedProbeLite> dialog(
        QStringLiteral("Pick SeedProbe"),
        { { QStringLiteral("ID"), [](const simcore::db::SeedProbeLite& row) { return QString::number(row.id); } },
          { QStringLiteral("Savestate"), [](const simcore::db::SeedProbeLite& row) { return QString::number(row.savestate_id); } },
          { QStringLiteral("Status"), [](const simcore::db::SeedProbeLite& row) { return QString::fromStdString(row.status); } } },
        [](const PagedQuery<>& query, const QString& search) {
            return DataService::FetchSeedProbesPage(query, search.toStdString(), true).get();
        },
        [](const simcore::db::SeedProbeLite& row) { return static_cast<qint64>(row.id); },
        [](const simcore::db::SeedProbeLite& row) {
            return QStringLiteral("SeedProbe %1 · savestate %2 · %3").arg(row.id).arg(row.savestate_id).arg(QString::fromStdString(row.status));
        },
        this);
    if (dialog.exec() == QDialog::Accepted && dialog.selectedRow().has_value()) {
        const qint64 id = dialog.selectedId();
        if (!selectedSeedProbeIds_.contains(id)) {
            selectedSeedProbeIds_.append(id);
            if (primarySeedProbeId_ <= 0) {
                primarySeedProbeId_ = id;
            }
            enqueueDeltaLoad(id);
        }
        syncIniFromWidgets();
    }
}

void JobBuilderPage::openExplorerSettingsPicker()
{
    LedgerPickerDialog<simcore::db::ExplorerSettingsLite> dialog(
        QStringLiteral("Pick ExplorerSettings"),
        { { QStringLiteral("ID"), [](const simcore::db::ExplorerSettingsLite& row) { return QString::number(row.id); } },
          { QStringLiteral("Name"), [](const simcore::db::ExplorerSettingsLite& row) { return QString::fromStdString(row.name); } },
          { QStringLiteral("Description"), [](const simcore::db::ExplorerSettingsLite& row) { return QString::fromStdString(row.description); } } },
        [](const PagedQuery<>& query, const QString& search) {
            return DataService::FetchExplorerSettingsPage(query, search.toStdString()).get();
        },
        [](const simcore::db::ExplorerSettingsLite& row) { return static_cast<qint64>(row.id); },
        [](const simcore::db::ExplorerSettingsLite& row) {
            return QStringLiteral("ExplorerSettings %1 · %2").arg(row.id).arg(QString::fromStdString(row.name));
        },
        this);
    if (dialog.exec() == QDialog::Accepted && dialog.selectedRow().has_value()) {
        const qint64 id = dialog.selectedId();
        if (!selectedExplorerSettingsIds_.contains(id)) {
            selectedExplorerSettingsIds_.append(id);
            if (primaryExplorerSettingsId_ <= 0) {
                primaryExplorerSettingsId_ = id;
            }
        }
        syncIniFromWidgets();
    }
}

void JobBuilderPage::requestPreview()
{
    if (!hasIni_) {
        return;
    }
    syncIniFromWidgets();
    previewBusy_ = true;
    preview_.reset();
    previewErrorMessage_.clear();
    refreshPreviewPanel();
    updateStatusMessage();
    const int programKind = selectedProgramKind();
    const IniDoc doc = ini_;
    previewWatcher_.setFuture(runAsync([programKind, doc]() {
        return PhaseBuilderService::Preview(programKind, doc);
    }));
}

void JobBuilderPage::requestSubmit()
{
    if (!canSubmit()) {
        return;
    }
    syncIniFromWidgets();
    submitBusy_ = true;
    submitErrorMessage_.clear();
    submitInfoMessage_.clear();
    refreshSubmitPanel();
    updateStatusMessage();

    const int originalProgramKind = selectedProgramKind();
    int programKind = originalProgramKind;
    if (programKind == PK_BattleTurnRunner && explorerSingleTurnCheck_->isChecked()) {
        programKind = PK_BattleSingleTurnRunner;
    }
    const IniDoc baseIni = ini_;
    const QString purpose = purposeEdit_->text().trimmed();
    const QString meta = metaEdit_->toPlainText().trimmed();
    const auto selectedSettings = selectedExplorerSettingsIds_;
    const auto selectedSeedProbes = selectedSeedProbeIds_;
    const auto selectedDeltas = selectedDeltaIdsByProbe_;
    const auto previewCopy = preview_;

    submitWatcher_.setFuture(runAsync([programKind, baseIni, purpose, meta, selectedSettings, selectedSeedProbes, selectedDeltas, previewCopy]() -> SubmitResultPayload {
        SubmitResultPayload out{};
        try {
            const std::optional<std::string> metaText = meta.isEmpty() ? std::optional<std::string>{} : std::optional<std::string>{ meta.toStdString() };
            if (programKind == PK_BattleTurnRunner || programKind == PK_BattleSingleTurnRunner) {
                std::vector<qint64> settings;
                for (qint64 id : selectedSettings) settings.push_back(id);
                std::vector<qint64> probes;
                for (qint64 id : selectedSeedProbes) probes.push_back(id);
                auto bp = simcore::db::codec::battle::run::BlueprintIni::from_section(baseIni);
                if (settings.empty() && bp.settings_id > 0) settings.push_back(bp.settings_id);
                if (probes.empty() && bp.seed_probe_id > 0) probes.push_back(bp.seed_probe_id);
                if (settings.empty() || probes.empty()) {
                    out.errorMessage = QStringLiteral("Select at least one settings row and one seed probe.");
                    return out;
                }

                qint64 firstJobSetId = 0;
                qint64 createdCount = 0;
                for (qint64 settingsId : settings) {
                    for (qint64 probeId : probes) {
                        IniDoc comboIni = baseIni;
                        auto comboBp = simcore::db::codec::battle::run::BlueprintIni::from_section(comboIni);
                        comboBp.settings_id = settingsId;
                        comboBp.seed_probe_id = probeId;
                        if (!selectedDeltas.contains(probeId) || selectedDeltas.value(probeId).isEmpty()) {
                            comboBp.delta_seed_ids_csv = "0";
                        } else {
                            QList<qint64> ids = selectedDeltas.value(probeId).values();
                            std::sort(ids.begin(), ids.end());
                            std::vector<int64_t> nativeIds;
                            nativeIds.reserve(ids.size());
                            for (qint64 id : ids) nativeIds.push_back(id);
                            comboBp.delta_seed_ids_csv = simcore::db::codec::battle::run::BlueprintIni::to_ids_csv(nativeIds);
                        }
                        comboBp.set_section(comboIni);

                        QString comboPurpose = purpose;
                        if (!comboPurpose.isEmpty()) comboPurpose += QStringLiteral(" | ");
                        comboPurpose += QStringLiteral("settings=%1 seed_probe=%2").arg(settingsId).arg(probeId);

                        const auto createResult = DataService::CreateJobSetAsync(
                            comboPurpose.isEmpty() ? std::optional<std::string>{} : std::optional<std::string>{ comboPurpose.toStdString() },
                            programKind, {}, {}, {}, metaText, {}, {}).get();
                        if (!createResult.ok) {
                            out.errorMessage = QString::fromStdString(createResult.error.message);
                            return out;
                        }

                        if (firstJobSetId <= 0) {
                            firstJobSetId = createResult.value;
                        }

                        const auto encodeResult = DataService::EncodeJobSetWithCodecAsync(programKind, createResult.value, comboIni.to_string_sorted(), {}).get();
                        if (!encodeResult.ok) {
                            out.errorMessage = QString::fromStdString(encodeResult.error.message);
                            return out;
                        }
                        ++createdCount;
                    }
                }
                out.ok = true;
                out.firstJobSetId = firstJobSetId;
                out.createdCount = createdCount;
                return out;
            }

            const auto createResult = DataService::CreateJobSetAsync(
                purpose.isEmpty() ? std::optional<std::string>{} : std::optional<std::string>{ purpose.toStdString() },
                programKind, {}, {}, {}, metaText, {}, {}).get();
            if (!createResult.ok) {
                out.errorMessage = QString::fromStdString(createResult.error.message);
                return out;
            }
            const auto encodeResult = DataService::EncodeJobSetWithCodecAsync(programKind, createResult.value, baseIni.to_string_sorted(), {}).get();
            if (!encodeResult.ok) {
                out.errorMessage = QString::fromStdString(encodeResult.error.message);
                return out;
            }
            if (programKind == PK_TasMovie && previewCopy.has_value() && previewCopy->tasmovie.has_value()) {
                const auto expectedResult = DataService::SetJobSetExpectedTotalAsync(createResult.value, previewCopy->tasmovie->jobs, {}).get();
                if (!expectedResult.ok) {
                    out.errorMessage = QString::fromStdString(expectedResult.error.message);
                    return out;
                }
            }
            out.ok = true;
            out.firstJobSetId = createResult.value;
            out.createdCount = 1;
            return out;
        } catch (...) {
            out.errorMessage = describeException("Submit failed");
            return out;
        }
    }));
}

void JobBuilderPage::enqueueDeltaLoad(qint64 probeId)
{
    using DeltaResult = simcore::db::DbResult<std::vector<simcore::db::DeltaSeedRow>>;
    auto* watcher = new QFutureWatcher<DeltaResult>(this);
    connect(watcher, &QFutureWatcher<DeltaResult>::finished, this, [this, probeId, watcher]() {
        try {
            const auto result = watcher->result();
            if (result.ok) {
                deltaRowsByProbe_.insert(probeId, QVector<simcore::db::DeltaSeedRow>(result.value.begin(), result.value.end()));
            } else {
                setInlineMessage(QStringLiteral("Failed to load unique deltas for SeedProbe %1: %2").arg(probeId).arg(QString::fromStdString(result.error.message)), QStringLiteral("error"));
            }
        } catch (...) {
            setInlineMessage(describeException("Failed to load unique deltas"), QStringLiteral("error"));
        }
        refreshDeltaTree();
        watcher->deleteLater();
    });
    watcher->setFuture(runAsync([probeId]() {
        return DeltaSeedRepo::ListUniqueForProbeAsync(probeId).get();
    }));
}

int JobBuilderPage::selectedProgramKind() const
{
    return kindCombo_->currentData().toInt();
}

QString JobBuilderPage::selectedProgramKindName() const
{
    return kindCombo_->currentText();
}

bool JobBuilderPage::canSubmit() const
{
    return hasIni_ && validationEntries_.isEmpty() && !submitBusy_;
}

QString JobBuilderPage::joinLines(const QVector<QString>& lines) const
{
    QString out;
    for (int index = 0; index < lines.size(); ++index) {
        if (index > 0) {
            out += QLatin1Char('\n');
        }
        out += lines.at(index);
    }
    return out;
}
