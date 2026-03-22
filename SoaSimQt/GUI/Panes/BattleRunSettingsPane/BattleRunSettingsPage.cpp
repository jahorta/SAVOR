#include "BattleRunSettingsPage.h"

#include "GUI/Widgets/LedgerPickerDialog.h"
#include "DB/AddressProgramRepo.h"
#include "DB/DBCore/ObjectStore.h"
#include "DB/ProgramDB/BattleContextDBCodec.h"
#include "DB/SavestateRepo.h"
#include "Phases/BattleExplorer.h"
#include "Phases/Programs/BattleRunner/BattleRunnerDBSettingsWriter.h"
#include "Runner/Breakpoints/BPRegistry.h"
#include "Runner/Breakpoints/Predicate.h"
#include "Utils/IniDoc.h"
#include "Core/Input/SoaBattle/ActionTypes.h"
#include "Core/Memory/Soa/Battle/BattleContextCodec.h"
#include "Core/Memory/Soa/SoaConstants.h"
#include "Core/Memory/Soa/SoaAddrProgram.h"
#include "PresetEditorDialog.h"
#include "ActionPresetDragTableModel.h"
#include "BattleRunSettingsDragDrop.h"
#include "UiActionSlotDropWidget.h"
#include "PredicateEditorDialog.h"
#include "TemplateSaveDialog.h"
#include "GUI/Widgets/BattleContextTreeWidget.h"

#include <QtCore/QStringList>
#include <QtGui/QStandardItem>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QTreeView>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <utility>

using simcore::battleexplorer::BattleExplorer;
using simcore::battleexplorer::TargetBindingKind;
using simcore::db::AuthoringTemplateLite;
using simcore::db::AuthoringTemplateRow;
using simcore::db::AuthoringTemplatesRepo;
using simcore::db::BattleContextRow;
using simcore::db::DataService;
using simcore::db::DbResult;
using simcore::db::JobsRepo;
using simcore::db::ObjectStore;
using simcore::db::PredicateSpecLite;
using simcore::db::PredicateSpecRepo;
using simcore::db::PredicateSpecRow;
using simcore::db::SavestateLite;
using simcore::db::SeedProbeLite;
using simcore::db::TurnActionPresetLite;
using simcore::db::TurnActionPresetRepo;
using simcore::db::TurnActionPresetRow;
using simcore::db::UiConfigRow;
using soa::battle::actions::BattleAction;

namespace {
constexpr int kLibraryLimit = 100;
constexpr char kUiConfigSectionName[] = "ui_config";
constexpr char kPredicateSectionName[] = "predicate";

quint32 invalidCellKey(const int turnIndex, const int actorSlot)
{
    return (static_cast<quint32>(turnIndex) << 8U) | static_cast<quint32>(actorSlot);
}

QString actionName(const int macro)
{
    switch (static_cast<BattleAction>(macro)) {
    case BattleAction::Attack: return QStringLiteral("Attack");
    case BattleAction::Defend: return QStringLiteral("Defend");
    case BattleAction::Focus: return QStringLiteral("Focus");
    case BattleAction::UseItem: return QStringLiteral("Use Item");
    case BattleAction::FakeAttack: return QStringLiteral("Fake Attack");
    }
    return QStringLiteral("Unknown");
}

QString targetKindName(const int targetKind)
{
    switch (static_cast<TargetBindingKind>(targetKind)) {
    case TargetBindingKind::SingleEnemy: return QStringLiteral("Single");
    case TargetBindingKind::MultipleEnemies: return QStringLiteral("Multiple");
    case TargetBindingKind::AnyEnemy: return QStringLiteral("Any");
    case TargetBindingKind::SameAsOtherPC: return QStringLiteral("Same-as-PC");
    }
    return QStringLiteral("Unknown");
}

QString itemName(const int itemId)
{
    if (itemId < 0 || static_cast<std::size_t>(itemId) >= soa::text::ItemNames.size()) {
        return QStringLiteral("(none)");
    }
    return QString::fromUtf8(soa::text::ItemNames.at(static_cast<std::size_t>(itemId)).data());
}

void configureFlatTreeView(QTreeView* view, const QString& objectName)
{
    view->setObjectName(objectName);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setAlternatingRowColors(true);
    view->setRootIsDecorated(false);
    view->setItemsExpandable(false);
    view->setAllColumnsShowFocus(true);
    view->setUniformRowHeights(true);
    view->setIndentation(0);
    view->header()->setStretchLastSection(true);
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


} // namespace

BattleRunSettingsPage::BattleRunSettingsPage(QWidget* parent)
    : QWidget(parent)
{
    createWidgets();
    wireSignals();
    loadInitialData();
}

BattleRunSettingsPage::~BattleRunSettingsPage() = default;

void BattleRunSettingsPage::createWidgets()
{
    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    inlineMessageLabel_ = new QLabel(this);
    inlineMessageLabel_->setObjectName("jobSetsInlineMessage");
    inlineMessageLabel_->setWordWrap(true);
    inlineMessageLabel_->hide();
    root->addWidget(inlineMessageLabel_);

    QHBoxLayout* columns = new QHBoxLayout();
    columns->setSpacing(12);
    root->addLayout(columns, 1);

    QWidget* left = new QWidget(this);
    QVBoxLayout* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(12);

    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("UI Actions"), left, &cardLayout);
        QHBoxLayout* toolbar = new QHBoxLayout();
        actionSearchEdit_ = new QLineEdit(card);
        actionSearchEdit_->setPlaceholderText(QStringLiteral("Search presets…"));
        actionRefreshButton_ = new QPushButton(QStringLiteral("Refresh"), card);
        actionNewButton_ = new QPushButton(QStringLiteral("New Preset…"), card);
        actionRefreshButton_->setObjectName("jobsSecondaryButton");
        actionNewButton_->setObjectName("jobsSecondaryButton");
        toolbar->addWidget(actionSearchEdit_, 1);
        toolbar->addWidget(actionRefreshButton_);
        toolbar->addWidget(actionNewButton_);
        cardLayout->addLayout(toolbar);
        actionTable_ = new QTreeView(card);
        configureFlatTreeView(actionTable_, QStringLiteral("battleRunSettingsActionTree"));
        actionTable_->setDragEnabled(true);
        actionTable_->setDragDropMode(QAbstractItemView::DragOnly);
        actionTable_->setDefaultDropAction(Qt::CopyAction);
        actionTableModel_ = new ActionPresetDragTableModel(this);
        actionTableModel_->setHorizontalHeaderLabels(QStringList{ QStringLiteral("ID"), QStringLiteral("Name"), QStringLiteral("Action") });
        actionTable_->setModel(actionTableModel_);
        actionTable_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        cardLayout->addWidget(actionTable_);
        leftLayout->addWidget(card);
    }

    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("Predicates"), left, &cardLayout);
        QHBoxLayout* toolbar = new QHBoxLayout();
        predicateSearchEdit_ = new QLineEdit(card);
        predicateSearchEdit_->setPlaceholderText(QStringLiteral("Search predicates…"));
        predicateRefreshButton_ = new QPushButton(QStringLiteral("Refresh"), card);
        predicateNewButton_ = new QPushButton(QStringLiteral("New Predicate…"), card);
        predicateRefreshButton_->setObjectName("jobsSecondaryButton");
        predicateNewButton_->setObjectName("jobsSecondaryButton");
        toolbar->addWidget(predicateSearchEdit_, 1);
        toolbar->addWidget(predicateRefreshButton_);
        toolbar->addWidget(predicateNewButton_);
        cardLayout->addLayout(toolbar);
        predicateTable_ = new QTreeView(card);
        configureFlatTreeView(predicateTable_, QStringLiteral("battleRunSettingsPredicateTree"));
        predicateTableModel_ = new QStandardItemModel(this);
        predicateTableModel_->setHorizontalHeaderLabels(QStringList{ QStringLiteral("ID"), QStringLiteral("Name"), QStringLiteral("Abort"), QStringLiteral("BP") });
        predicateTable_->setModel(predicateTableModel_);
        predicateTable_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        cardLayout->addWidget(predicateTable_);
        leftLayout->addWidget(card);
    }

    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("Authoring Templates"), left, &cardLayout);
        QHBoxLayout* toolbar = new QHBoxLayout();
        templateSearchEdit_ = new QLineEdit(card);
        templateSearchEdit_->setPlaceholderText(QStringLiteral("Search templates…"));
        templateRefreshButton_ = new QPushButton(QStringLiteral("Refresh"), card);
        templateRefreshButton_->setObjectName("jobsSecondaryButton");
        toolbar->addWidget(templateSearchEdit_, 1);
        toolbar->addWidget(templateRefreshButton_);
        cardLayout->addLayout(toolbar);
        templateTable_ = new QTreeView(card);
        configureFlatTreeView(templateTable_, QStringLiteral("battleRunSettingsTemplateTree"));
        templateTableModel_ = new QStandardItemModel(this);
        templateTableModel_->setHorizontalHeaderLabels(QStringList{ QStringLiteral("ID"), QStringLiteral("Name") });
        templateTable_->setModel(templateTableModel_);
        templateTable_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        cardLayout->addWidget(templateTable_);
        leftLayout->addWidget(card);
    }

    QWidget* middle = new QWidget(this);
    QVBoxLayout* middleLayout = new QVBoxLayout(middle);
    middleLayout->setContentsMargins(0, 0, 0, 0);
    middleLayout->setSpacing(12);

    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("UI Config"), middle, &cardLayout);
        QHBoxLayout* toolbar = new QHBoxLayout();
        addTurnButton_ = new QPushButton(QStringLiteral("Add Turn"), card);
        addTurnButton_->setObjectName("jobsSecondaryButton");
        toolbar->addStretch();
        toolbar->addWidget(addTurnButton_);
        cardLayout->addLayout(toolbar);

        QScrollArea* scrollArea = new QScrollArea(card);
        scrollArea->setWidgetResizable(true);
        uiConfigContainer_ = new QWidget(scrollArea);
        uiConfigLayout_ = new QVBoxLayout(uiConfigContainer_);
        uiConfigLayout_->setContentsMargins(0, 0, 0, 0);
        uiConfigLayout_->setSpacing(12);
        uiConfigLayout_->addStretch();
        scrollArea->setWidget(uiConfigContainer_);
        cardLayout->addWidget(scrollArea, 1);
        middleLayout->addWidget(card, 1);
    }

    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("Selected Predicates"), middle, &cardLayout);
        predicateDraftList_ = new QListWidget(card);
        cardLayout->addWidget(predicateDraftList_);
        QHBoxLayout* actions = new QHBoxLayout();
        addPredicateButton_ = new QPushButton(QStringLiteral("Add Predicate…"), card);
        movePredicateUpButton_ = new QPushButton(QStringLiteral("Up"), card);
        movePredicateDownButton_ = new QPushButton(QStringLiteral("Down"), card);
        editPredicateButton_ = new QPushButton(QStringLiteral("Edit…"), card);
        removePredicateButton_ = new QPushButton(QStringLiteral("Remove"), card);
        for (QPushButton* button : { addPredicateButton_, movePredicateUpButton_, movePredicateDownButton_, editPredicateButton_, removePredicateButton_ }) {
            button->setObjectName("jobsSecondaryButton");
            actions->addWidget(button);
        }
        actions->addStretch();
        cardLayout->addLayout(actions);
        middleLayout->addWidget(card);
    }

    QWidget* right = new QWidget(this);
    QVBoxLayout* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(12);

    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("Battle Context"), right, &cardLayout);
        contextSummaryLabel_ = new QLabel(card);
        contextSummaryLabel_->setWordWrap(true);
        cardLayout->addWidget(contextSummaryLabel_);
        contextTree_ = new BattleContextTreeWidget(card);
        contextTree_->setMinimumHeight(220);
        cardLayout->addWidget(contextTree_);
        QGridLayout* grid = new QGridLayout();
        pickSavestateButton_ = new QPushButton(QStringLiteral("Pick Savestate…"), card);
        pickSeedProbeButton_ = new QPushButton(QStringLiteral("Pick Seed Probe…"), card);
        clearContextButton_ = new QPushButton(QStringLiteral("Clear Context"), card);
        getContextButton_ = new QPushButton(QStringLiteral("Get Context Update"), card);
        for (QPushButton* button : { pickSavestateButton_, pickSeedProbeButton_, clearContextButton_, getContextButton_ }) {
            button->setObjectName("jobsSecondaryButton");
        }
        grid->addWidget(pickSavestateButton_, 0, 0);
        grid->addWidget(pickSeedProbeButton_, 0, 1);
        grid->addWidget(getContextButton_, 1, 0);
        grid->addWidget(clearContextButton_, 1, 1);
        cardLayout->addLayout(grid);
        rightLayout->addWidget(card);
    }

    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("Counts & Estimate"), right, &cardLayout);
        countsLabel_ = new QLabel(card);
        countsLabel_->setWordWrap(true);
        estimateLabel_ = new QLabel(card);
        estimateLabel_->setWordWrap(true);
        cardLayout->addWidget(countsLabel_);
        cardLayout->addWidget(estimateLabel_);
        rightLayout->addWidget(card);
    }

    {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(QStringLiteral("Save / Materialize"), right, &cardLayout);
        settingsNameEdit_ = new QLineEdit(card);
        settingsDescriptionEdit_ = new QPlainTextEdit(card);
        settingsDescriptionEdit_->setMaximumHeight(100);
        saveSettingsButton_ = new QPushButton(QStringLiteral("Save Explorer Settings"), card);
        saveTemplateButton_ = new QPushButton(QStringLiteral("Save Authoring Template…"), card);
        saveSettingsButton_->setObjectName("jobsPrimaryButton");
        saveTemplateButton_->setObjectName("jobsSecondaryButton");
        cardLayout->addWidget(new QLabel(QStringLiteral("Settings name"), card));
        cardLayout->addWidget(settingsNameEdit_);
        cardLayout->addWidget(new QLabel(QStringLiteral("Settings description"), card));
        cardLayout->addWidget(settingsDescriptionEdit_);
        cardLayout->addWidget(saveSettingsButton_);
        cardLayout->addWidget(saveTemplateButton_);
        rightLayout->addWidget(card);
        rightLayout->addStretch();
    }

    columns->addWidget(left, 1);
    columns->addWidget(middle, 1);
    columns->addWidget(right, 1);
}

void BattleRunSettingsPage::wireSignals()
{
    connect(actionRefreshButton_, &QPushButton::clicked, this, [this]() { refreshUiActionLibrary(); });
    connect(predicateRefreshButton_, &QPushButton::clicked, this, [this]() { refreshPredicateLibrary(); });
    connect(templateRefreshButton_, &QPushButton::clicked, this, [this]() { refreshTemplateLibrary(); });
    connect(actionNewButton_, &QPushButton::clicked, this, [this]() { openAddPresetDialog(); });
    connect(predicateNewButton_, &QPushButton::clicked, this, [this]() { openAddPredicateDialog(); });
    connect(addTurnButton_, &QPushButton::clicked, this, [this]() { addTurn(); });

    connect(actionTable_, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        const int row = index.row();
        if (row >= 0 && row < uiActionResults_.size()) {
            openAddPresetDialog(std::nullopt, uiActionResults_.at(row).id);
        }
    });
    connect(predicateTable_, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        const int row = index.row();
        if (row >= 0 && row < predicateResults_.size()) {
            const PredicateSpecLite& lite = predicateResults_.at(row);
            predicates_.push_back({ lite.id, QString::fromStdString(lite.name), QString::fromStdString(lite.description) });
            refreshPredicateDraftView();
        }
    });
    connect(templateTable_, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        const int row = index.row();
        if (row >= 0 && row < templateResults_.size()) {
            loadAuthoringTemplate(templateResults_.at(row).id);
        }
    });

    connect(addPredicateButton_, &QPushButton::clicked, this, [this]() { openAddPredicateDialog(); });
    connect(editPredicateButton_, &QPushButton::clicked, this, [this]() {
        const int row = predicateDraftList_->currentRow();
        if (row >= 0 && row < predicates_.size()) {
            openAddPredicateDialog(row, predicates_.at(row).predicateId);
        }
    });
    connect(removePredicateButton_, &QPushButton::clicked, this, [this]() {
        const int row = predicateDraftList_->currentRow();
        if (row >= 0 && row < predicates_.size()) {
            predicates_.removeAt(row);
            refreshPredicateDraftView();
        }
    });
    connect(movePredicateUpButton_, &QPushButton::clicked, this, [this]() {
        const int row = predicateDraftList_->currentRow();
        if (row > 0 && row < predicates_.size()) {
            predicates_.swapItemsAt(row, row - 1);
            refreshPredicateDraftView();
            predicateDraftList_->setCurrentRow(row - 1);
        }
    });
    connect(movePredicateDownButton_, &QPushButton::clicked, this, [this]() {
        const int row = predicateDraftList_->currentRow();
        if (row >= 0 && row + 1 < predicates_.size()) {
            predicates_.swapItemsAt(row, row + 1);
            refreshPredicateDraftView();
            predicateDraftList_->setCurrentRow(row + 1);
        }
    });

    connect(pickSavestateButton_, &QPushButton::clicked, this, [this]() { openSavestatePicker(); });
    connect(pickSeedProbeButton_, &QPushButton::clicked, this, [this]() { openSeedProbePicker(); });
    connect(clearContextButton_, &QPushButton::clicked, this, [this]() { clearBattleContext(); });
    connect(getContextButton_, &QPushButton::clicked, this, [this]() { requestFreshBattleContext(); });
    connect(saveSettingsButton_, &QPushButton::clicked, this, [this]() { saveExplorerSettings(); });
    connect(saveTemplateButton_, &QPushButton::clicked, this, [this]() { openSaveTemplateDialog(); });

    connect(&actionListWatcher_, &QFutureWatcher<ActionListResult>::finished, this, [this]() {
        const ActionListResult result = actionListWatcher_.result();
        if (result.ok) {
            uiActionResults_.clear();
            for (const TurnActionPresetLite& row : result.value) {
                uiActionResults_.push_back(row);
            }
            setInfoMessage(QStringLiteral("Updated UI action library."));
        } else {
            setErrorMessage(QStringLiteral("Failed to load UI action presets: %1").arg(QString::fromStdString(result.error.message)));
        }
        refreshUiActionLibraryView();
    });

    connect(&predicateListWatcher_, &QFutureWatcher<PredicateListResult>::finished, this, [this]() {
        const PredicateListResult result = predicateListWatcher_.result();
        if (result.ok) {
            predicateResults_.clear();
            for (const PredicateSpecLite& row : result.value) {
                predicateResults_.push_back(row);
                predicateLiteCache_.insert(row.id, row);
            }
            setInfoMessage(QStringLiteral("Updated predicate library."));
        } else {
            setErrorMessage(QStringLiteral("Failed to load predicates: %1").arg(QString::fromStdString(result.error.message)));
        }
        refreshPredicateLibraryView();
    });

    connect(&templateListWatcher_, &QFutureWatcher<TemplateListResult>::finished, this, [this]() {
        const TemplateListResult result = templateListWatcher_.result();
        if (result.ok) {
            templateResults_.clear();
            for (const AuthoringTemplateLite& row : result.value) {
                templateResults_.push_back(row);
            }
            setInfoMessage(QStringLiteral("Updated authoring templates."));
        } else {
            setErrorMessage(QStringLiteral("Failed to load authoring templates: %1").arg(QString::fromStdString(result.error.message)));
        }
        refreshTemplateLibraryView();
    });

    connect(&contextLookupWatcher_, &QFutureWatcher<ContextLookupResult>::finished, this, [this]() {
        const ContextLookupResult result = contextLookupWatcher_.result();
        if (!result.ok) {
            hasContext_ = false;
            contextState_ = ContextState::Failure;
            setErrorMessage(QStringLiteral("Failed to load BattleContext row: %1").arg(QString::fromStdString(result.error.message)));
            refreshContextPanel();
            return;
        }
        if (!result.value.has_value()) {
            hasContext_ = false;
            contextCodecVersion_ = 0;
            contextState_ = ContextState::None;
            setInfoMessage(QStringLiteral("No BattleContext exists yet for the selected savestate."));
            refreshContextPanel();
            return;
        }
        handleBattleContextLoaded(*result.value);
    });

    connect(&contextDecodeWatcher_, &QFutureWatcher<TextResult>::finished, this, [this]() {
        const TextResult result = contextDecodeWatcher_.result();
        if (!result.ok) {
            hasContext_ = false;
            contextState_ = ContextState::Failure;
            setErrorMessage(QStringLiteral("Failed to decode BattleContext artifact: %1").arg(QString::fromStdString(result.error.message)));
            refreshContextPanel();
            return;
        }
        hasContext_ = soa::battle::ctx::codec::decode(result.value, battleContext_);
        if (!hasContext_) {
            contextState_ = ContextState::Failure;
            setErrorMessage(QStringLiteral("BattleContext artifact could not be decoded."));
            refreshContextPanel();
            return;
        }
        contextState_ = ContextState::Decoded;
        reconcilePartySize();
        validateGridAgainstContext();
        computeEstimate();
        setInfoMessage(QStringLiteral("BattleContext loaded."));
        refreshAllViews();
    });

    connect(&savestateForSeedProbeWatcher_, &QFutureWatcher<simcore::db::DbResult<qint64>>::finished, this, [this]() {
        const auto result = savestateForSeedProbeWatcher_.result();
        if (!result.ok) {
            contextState_ = ContextState::Failure;
            setErrorMessage(QStringLiteral("Failed to resolve savestate for seed probe: %1").arg(QString::fromStdString(result.error.message)));
            refreshContextPanel();
            return;
        }
        requestBattleContextForSavestate(result.value);
    });

    connect(&freshContextJobWatcher_, &QFutureWatcher<simcore::db::DbResult<qint64>>::finished, this, [this]() {
        const auto result = freshContextJobWatcher_.result();
        if (!result.ok) {
            contextState_ = ContextState::Failure;
            setErrorMessage(QStringLiteral("Failed to queue BattleContext request: %1").arg(QString::fromStdString(result.error.message)));
            refreshContextPanel();
            return;
        }
        contextJobId_ = result.value;
        contextState_ = ContextState::Queued;
        contextPollTimer_.start(500);
        refreshContextPanel();
    });

    connect(&contextPollTimer_, &QTimer::timeout, this, [this]() { pollBattleContextJob(); });
    connect(&contextPollWatcher_, &QFutureWatcher<simcore::db::DbResult<simcore::db::JobRow>>::finished, this, [this]() {
        const auto result = contextPollWatcher_.result();
        if (!result.ok) {
            contextPollTimer_.stop();
            contextState_ = ContextState::Failure;
            setErrorMessage(QStringLiteral("Failed to poll BattleContext job: %1").arg(QString::fromStdString(result.error.message)));
            refreshContextPanel();
            return;
        }

        if (result.value.state == "RUNNING") {
            contextState_ = ContextState::Running;
        } else if (result.value.state == "QUEUED") {
            contextState_ = ContextState::Queued;
        } else if (result.value.state == "SUCCEEDED") {
            contextPollTimer_.stop();
            contextState_ = ContextState::Success;
            requestBattleContextForSavestate(savestateId_);
            return;
        } else if (result.value.state == "FAILED") {
            contextPollTimer_.stop();
            contextState_ = ContextState::Failure;
        }
        refreshContextPanel();
    });

    connect(&saveTemplateWatcher_, &QFutureWatcher<SaveTemplateResult>::finished, this, [this]() {
        const SaveTemplateResult result = saveTemplateWatcher_.result();
        if (result.ok) {
            setInfoMessage(QStringLiteral("Authoring template saved as ID %1.").arg(result.value));
            refreshTemplateLibrary();
        } else {
            setErrorMessage(QStringLiteral("Failed to save authoring template: %1").arg(QString::fromStdString(result.error.message)));
        }
        refreshSavePanel();
    });

    connect(&saveSettingsWatcher_, &QFutureWatcher<SaveSettingsResult>::finished, this, [this]() {
        const SaveSettingsResult result = saveSettingsWatcher_.result();
        if (result.ok) {
            setInfoMessage(QStringLiteral("Explorer settings saved as ID %1.").arg(result.value));
        } else {
            setErrorMessage(QStringLiteral("Failed to save explorer settings: %1").arg(QString::fromStdString(result.error.message)));
        }
        refreshSavePanel();
    });
}

void BattleRunSettingsPage::loadInitialData()
{
    addTurn();
    refreshUiActionLibrary();
    refreshPredicateLibrary();
    refreshTemplateLibrary();
    refreshAllViews();
}

void BattleRunSettingsPage::refreshUiActionLibrary()
{
    if (actionListWatcher_.isRunning()) {
        return;
    }
    const QString search = actionSearchEdit_->text().trimmed();
    actionListWatcher_.setFuture(QtConcurrent::run([search]() {
        return DataService::ListActionPresetsAsync(search.toStdString(), kLibraryLimit).get();
    }));
}

void BattleRunSettingsPage::refreshPredicateLibrary()
{
    if (predicateListWatcher_.isRunning()) {
        return;
    }
    const QString search = predicateSearchEdit_->text().trimmed();
    predicateListWatcher_.setFuture(QtConcurrent::run([search]() {
        return DataService::ListPredicateSpecsAsync(search.toStdString(), kLibraryLimit).get();
    }));
}

void BattleRunSettingsPage::refreshTemplateLibrary()
{
    if (templateListWatcher_.isRunning()) {
        return;
    }
    const QString search = templateSearchEdit_->text().trimmed();
    templateListWatcher_.setFuture(QtConcurrent::run([search]() {
        return AuthoringTemplatesRepo::ListLiteAsync(search.toStdString(), kLibraryLimit).get();
    }));
}

void BattleRunSettingsPage::refreshAllViews()
{
    refreshUiActionLibraryView();
    refreshPredicateLibraryView();
    refreshTemplateLibraryView();
    refreshDraftViews();
    refreshContextPanel();
    refreshEstimatePanel();
    refreshSavePanel();
    refreshInlineMessage();
}

void BattleRunSettingsPage::refreshUiActionLibraryView()
{
    actionTableModel_->removeRows(0, actionTableModel_->rowCount());
    for (const TurnActionPresetLite& item : uiActionResults_) {
        QList<QStandardItem*> rowItems;
        auto* idItem = new QStandardItem(QString::number(item.id));
        auto* nameItem = new QStandardItem(QString::fromStdString(item.name));
        auto* actionItem = new QStandardItem(actionName(item.macro));
        idItem->setData(item.id, battlerunsettings::kPresetIdRole);
        rowItems << idItem
                 << nameItem
                 << actionItem;
        actionTableModel_->appendRow(rowItems);
    }
}

void BattleRunSettingsPage::refreshPredicateLibraryView()
{
    predicateTableModel_->removeRows(0, predicateTableModel_->rowCount());
    for (const PredicateSpecLite& item : predicateResults_) {
        QList<QStandardItem*> rowItems;
        auto* idItem = new QStandardItem(QString::number(item.id));
        auto* nameItem = new QStandardItem(QString::fromStdString(item.name));
        auto* abortItem = new QStandardItem(item.abort_on_fail ? QStringLiteral("Yes") : QStringLiteral("No"));
        auto* bpItem = new QStandardItem(QStringLiteral("0x%1").arg(item.required_bp.pc, 8, 16, QLatin1Char('0')));
        nameItem->setToolTip(QString::fromStdString(item.description));
        rowItems << idItem << nameItem << abortItem << bpItem;
        predicateTableModel_->appendRow(rowItems);
    }
}

void BattleRunSettingsPage::refreshTemplateLibraryView()
{
    templateTableModel_->removeRows(0, templateTableModel_->rowCount());
    for (const AuthoringTemplateLite& item : templateResults_) {
        QList<QStandardItem*> rowItems;
        rowItems << new QStandardItem(QString::number(item.id))
                 << new QStandardItem(QString::fromStdString(item.name));
        templateTableModel_->appendRow(rowItems);
    }
}

void BattleRunSettingsPage::refreshDraftViews()
{
    refreshUiConfigEditor();
    refreshPredicateDraftView();
}

void BattleRunSettingsPage::refreshUiConfigEditor()
{
    while (QLayoutItem* item = uiConfigLayout_->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    for (int turnIndex = 0; turnIndex < uiConfig_.actions.size(); ++turnIndex) {
        QGroupBox* group = new QGroupBox(QStringLiteral("Turn %1").arg(turnIndex + 1), uiConfigContainer_);
        QVBoxLayout* groupLayout = new QVBoxLayout(group);

        QPushButton* deleteTurnButton = new QPushButton(QStringLiteral("Delete Turn"), group);
        deleteTurnButton->setObjectName("jobsSecondaryButton");
        connect(deleteTurnButton, &QPushButton::clicked, this, [this, turnIndex]() {
            if (turnIndex >= 0 && turnIndex < uiConfig_.actions.size()) {
                uiConfig_.actions.removeAt(turnIndex);
                validateGridAgainstContext();
                computeEstimate();
                refreshAllViews();
            }
        });
        groupLayout->addWidget(deleteTurnButton, 0, Qt::AlignRight);

        for (const UiActionInstance& instance : uiConfig_.actions.at(turnIndex)) {
            const int actorSlot = static_cast<int>(instance.actorSlot);
            UiActionSlotDropWidget* rowFrame = new UiActionSlotDropWidget(turnIndex, actorSlot, group);
            rowFrame->setFrameShape(QFrame::StyledPanel);
            rowFrame->setStyleSheet(QStringLiteral("QFrame[dropActive=\"true\"] { border: 2px solid #4f9dff; background-color: rgba(79, 157, 255, 0.12); }"));
            rowFrame->setToolTip(QStringLiteral("Drag a UI action preset here to assign it to this slot."));
            QHBoxLayout* rowLayout = new QHBoxLayout(rowFrame);
            QLabel* label = new QLabel(QStringLiteral("Slot %1").arg(actorSlot), rowFrame);
            QLabel* summary = new QLabel(presetSummary(instance.presetId), rowFrame);
            summary->setWordWrap(true);
            if (invalidCells_.contains(invalidCellKey(turnIndex, actorSlot))) {
                summary->setStyleSheet(QStringLiteral("color: #d9534f;"));
                summary->setToolTip(invalidReason(turnIndex, actorSlot));
            }
            QPushButton* assignButton = new QPushButton(QStringLiteral("Assign…"), rowFrame);
            QPushButton* editButton = new QPushButton(QStringLiteral("Edit…"), rowFrame);
            QPushButton* clearButton = new QPushButton(QStringLiteral("Clear"), rowFrame);
            for (QPushButton* button : { assignButton, editButton, clearButton }) {
                button->setObjectName("jobsSecondaryButton");
            }
            rowLayout->addWidget(label);
            rowLayout->addWidget(summary, 1);
            rowLayout->addWidget(assignButton);
            rowLayout->addWidget(editButton);
            rowLayout->addWidget(clearButton);
            connect(rowFrame, &UiActionSlotDropWidget::presetDropped, this, [this](const int droppedTurnIndex, const int droppedActorSlot, const qint64 presetId) {
                assignPresetToCell(droppedTurnIndex, droppedActorSlot, presetId);
            });
            connect(assignButton, &QPushButton::clicked, this, [this, turnIndex, actorSlot]() {
                openAddPresetDialog(std::make_pair(turnIndex, actorSlot));
            });
            connect(editButton, &QPushButton::clicked, this, [this, turnIndex, actorSlot, presetId = instance.presetId]() {
                openAddPresetDialog(std::make_pair(turnIndex, actorSlot), presetId > 0 ? std::optional<qint64>(presetId) : std::nullopt);
            });
            connect(clearButton, &QPushButton::clicked, this, [this, turnIndex, actorSlot]() {
                clearPresetAtCell(turnIndex, actorSlot);
            });
            groupLayout->addWidget(rowFrame);
        }

        uiConfigLayout_->addWidget(group);
    }
    uiConfigLayout_->addStretch();
}

void BattleRunSettingsPage::refreshPredicateDraftView()
{
    predicateDraftList_->clear();
    for (const PredicateDraft& draft : predicates_) {
        QListWidgetItem* item = new QListWidgetItem(QStringLiteral("Predicate #%1 · %2").arg(draft.predicateId).arg(draft.name), predicateDraftList_);
        item->setToolTip(draft.description);
    }
}

void BattleRunSettingsPage::refreshContextPanel()
{
    contextSummaryLabel_->setText(contextStateText());
    contextTree_->setBattleContext(seedProbeId_, savestateId_, partySize_, hasContext_, battleContext_);
    getContextButton_->setEnabled(savestateId_ > 0 && (!freshContextJobWatcher_.isRunning()) && (!contextPollTimer_.isActive()));
}

void BattleRunSettingsPage::refreshEstimatePanel()
{
    int turns = uiConfig_.actions.size();
    int filled = 0;
    for (const QVector<UiActionInstance>& turn : uiConfig_.actions) {
        for (const UiActionInstance& instance : turn) {
            if (instance.presetId > 0) {
                ++filled;
            }
        }
    }
    countsLabel_->setText(QStringLiteral("Turns: %1\nUI actions filled: %2\nPredicates: %3\nParty size: %4")
        .arg(turns)
        .arg(filled)
        .arg(predicates_.size())
        .arg(partySize_));

    if (!estimateState_.available) {
        estimateLabel_->setText(estimateState_.message.isEmpty() ? QStringLiteral("Estimate unavailable.") : estimateState_.message);
        return;
    }
    estimateLabel_->setText(QStringLiteral("Base plans: %1")
        .arg(estimateState_.basePlans));
}

void BattleRunSettingsPage::refreshSavePanel()
{
    const bool canSave = hasContext_ && allSlotsFilled() && !settingsNameEdit_->text().trimmed().isEmpty() && !settingsDescriptionEdit_->toPlainText().trimmed().isEmpty();
    saveSettingsButton_->setEnabled(canSave && !saveSettingsWatcher_.isRunning());
    saveTemplateButton_->setEnabled(!uiConfig_.actions.isEmpty() && !saveTemplateWatcher_.isRunning());
}

void BattleRunSettingsPage::refreshInlineMessage()
{
    QString text;
    if (!errorMessage_.isEmpty()) {
        text = errorMessage_;
        inlineMessageLabel_->setProperty("severity", QStringLiteral("error"));
    } else if (!infoMessage_.isEmpty()) {
        text = infoMessage_;
        inlineMessageLabel_->setProperty("severity", QStringLiteral("info"));
    }
    inlineMessageLabel_->setText(text);
    inlineMessageLabel_->setVisible(!text.isEmpty());
    style()->unpolish(inlineMessageLabel_);
    style()->polish(inlineMessageLabel_);
}

void BattleRunSettingsPage::addTurn()
{
    QVector<UiActionInstance> turn;
    turn.reserve(partySize_ > 0 ? partySize_ : 4);
    for (int slot = 0; slot < (partySize_ > 0 ? partySize_ : 4); ++slot) {
        turn.push_back({ static_cast<quint32>(slot), 0 });
    }
    uiConfig_.actions.push_back(turn);
    refreshDraftViews();
    refreshSavePanel();
}

void BattleRunSettingsPage::reconcilePartySize()
{
    int detected = 0;
    for (const auto& slot : battleContext_.slots_) {
        if (slot.is_alive && slot.is_player) {
            ++detected;
        }
    }
    if (detected <= 0) {
        return;
    }
    partySize_ = detected;
    for (QVector<UiActionInstance>& turn : uiConfig_.actions) {
        while (turn.size() > partySize_) {
            turn.removeLast();
        }
        while (turn.size() < partySize_) {
            turn.push_back({ static_cast<quint32>(turn.size()), 0 });
        }
    }
}

void BattleRunSettingsPage::assignPresetToCell(int turnIndex, int actorSlot, qint64 presetId)
{
    if (turnIndex < 0 || turnIndex >= uiConfig_.actions.size()) {
        return;
    }
    QVector<UiActionInstance>& turn = uiConfig_.actions[turnIndex];
    if (actorSlot < 0 || actorSlot >= turn.size()) {
        return;
    }
    turn[actorSlot].presetId = presetId;
    const auto presetResult = TurnActionPresetRepo::Get(presetId);
    if (presetResult.ok) {
        presetCache_.insert(presetId, presetResult.value);
    }
    validateGridAgainstContext();
    computeEstimate();
    refreshAllViews();
}

void BattleRunSettingsPage::clearPresetAtCell(int turnIndex, int actorSlot)
{
    if (turnIndex < 0 || turnIndex >= uiConfig_.actions.size()) {
        return;
    }
    QVector<UiActionInstance>& turn = uiConfig_.actions[turnIndex];
    if (actorSlot < 0 || actorSlot >= turn.size()) {
        return;
    }
    turn[actorSlot].presetId = 0;
    validateGridAgainstContext();
    computeEstimate();
    refreshAllViews();
}

void BattleRunSettingsPage::validateGridAgainstContext()
{
    invalidCells_.clear();
    invalidReasons_.clear();
    if (!hasContext_) {
        return;
    }

    quint32 presentEnemySlotsMask = 0;
    for (int slot = 4; slot <= 11; ++slot) {
        if (battleContext_.slots_[slot].present == 1) {
            presentEnemySlotsMask |= (1u << slot);
        }
    }

    QSet<quint16> presentItems;
    for (int index = 0; index < 80; ++index) {
        if (battleContext_.state.useable_items[index].item_id != 0xFFFF) {
            presentItems.insert(static_cast<quint16>(battleContext_.state.useable_items[index].item_id));
        }
    }

    for (int turnIndex = 0; turnIndex < uiConfig_.actions.size(); ++turnIndex) {
        const QVector<UiActionInstance>& turn = uiConfig_.actions.at(turnIndex);
        for (const UiActionInstance& instance : turn) {
            if (instance.presetId <= 0) {
                continue;
            }
            TurnActionPresetRow preset;
            if (presetCache_.contains(instance.presetId)) {
                preset = presetCache_.value(instance.presetId);
            } else {
                const auto result = TurnActionPresetRepo::Get(instance.presetId);
                if (!result.ok) {
                    continue;
                }
                preset = result.value;
                presetCache_.insert(instance.presetId, preset);
            }

            auto putBad = [&](const QString& reason) {
                const quint32 key = invalidCellKey(turnIndex, static_cast<int>(instance.actorSlot));
                invalidCells_.insert(key);
                invalidReasons_.insert(key, reason);
            };

            if (static_cast<BattleAction>(preset.macro) == BattleAction::UseItem && !presentItems.contains(static_cast<quint16>(preset.item_id))) {
                putBad(QStringLiteral("Item not present in inventory."));
                continue;
            }

            if (!preset.target_expr_ini.empty()) {
                const IniDoc ini = IniDoc::parse(preset.target_expr_ini);
                if (ini.get("target", "kind", "") == "ByEnemyKind") {
                    const int wantId = static_cast<int>(ini.get_i64("target", "enemy_kind_id", -1));
                    quint32 domainMask = 0;
                    for (int slot = 4; slot <= 11; ++slot) {
                        if (battleContext_.slots_[slot].present == 1 && static_cast<int>(battleContext_.slots_[slot].id) == wantId) {
                            domainMask |= (1u << slot);
                        }
                    }
                    if (domainMask == 0) {
                        putBad(QStringLiteral("No enemies of the selected kind are present."));
                    }
                }
                continue;
            }

            const auto kind = static_cast<TargetBindingKind>(preset.target_kind);
            if (kind == TargetBindingKind::SingleEnemy) {
                if ((presentEnemySlotsMask & (1u << preset.single_slot)) == 0) {
                    putBad(QStringLiteral("Selected enemy slot is not present."));
                }
            } else if (kind == TargetBindingKind::MultipleEnemies) {
                if ((static_cast<quint32>(preset.mask_bits) & presentEnemySlotsMask) == 0) {
                    putBad(QStringLiteral("No selected enemy slots are present."));
                }
            } else if (kind == TargetBindingKind::SameAsOtherPC) {
                const int slot = preset.same_as_pc;
                if (slot < 0 || slot > 3 || battleContext_.slots_[slot].present != 1) {
                    putBad(QStringLiteral("Selected PC slot is not present."));
                }
            }
        }
    }
}

bool BattleRunSettingsPage::allSlotsFilled() const
{
    if (partySize_ <= 0 || uiConfig_.actions.isEmpty()) {
        return false;
    }
    for (const QVector<UiActionInstance>& turn : uiConfig_.actions) {
        for (const UiActionInstance& instance : turn) {
            if (instance.presetId <= 0) {
                return false;
            }
        }
    }
    return true;
}

void BattleRunSettingsPage::openSavestatePicker()
{
    LedgerPickerDialog<SavestateLite> dialog(
        QStringLiteral("Pick Savestate"),
        { { QStringLiteral("ID"), [](const SavestateLite& row) { return QString::number(row.id); } },
          { QStringLiteral("Type"), [](const SavestateLite& row) { return QString::number(row.savestate_type); } },
          { QStringLiteral("Note"), [](const SavestateLite& row) { return QString::fromStdString(row.note); }, 2 } },
        [](const PagedQuery<>& query, const QString& search) {
            return DataService::FetchSavestatesPage(query, search.toStdString()).get();
        },
        [](const SavestateLite& row) { return static_cast<qint64>(row.id); },
        [](const SavestateLite& row) {
            return QStringLiteral("Savestate %1 · type %2 · %3").arg(row.id).arg(row.savestate_type).arg(QString::fromStdString(row.note));
        },
        this);
    if (dialog.exec() == QDialog::Accepted && dialog.selectedId() > 0) {
        requestBattleContextForSavestate(dialog.selectedId());
    }
}

void BattleRunSettingsPage::openSeedProbePicker()
{
    LedgerPickerDialog<SeedProbeLite> dialog(
        QStringLiteral("Pick SeedProbe"),
        { { QStringLiteral("ID"), [](const SeedProbeLite& row) { return QString::number(row.id); } },
          { QStringLiteral("Savestate"), [](const SeedProbeLite& row) { return QString::number(row.savestate_id); } },
          { QStringLiteral("Status"), [](const SeedProbeLite& row) { return QString::fromStdString(row.status); } } },
        [](const PagedQuery<>& query, const QString& search) {
            return DataService::FetchSeedProbesPage(query, search.toStdString(), true).get();
        },
        [](const SeedProbeLite& row) { return static_cast<qint64>(row.id); },
        [](const SeedProbeLite& row) {
            return QStringLiteral("SeedProbe %1 · savestate %2 · %3").arg(row.id).arg(row.savestate_id).arg(QString::fromStdString(row.status));
        },
        this);
    if (dialog.exec() == QDialog::Accepted && dialog.selectedId() > 0) {
        requestBattleContextForSeedProbe(dialog.selectedId());
    }
}

void BattleRunSettingsPage::requestBattleContextForSavestate(qint64 savestateId)
{
    savestateId_ = savestateId;
    contextState_ = ContextState::LoadingFromSavestate;
    if (!contextLookupWatcher_.isRunning()) {
        contextLookupWatcher_.setFuture(QtConcurrent::run([savestateId]() {
            return DataService::GetLatestBattleContextForSavestateAsync(savestateId).get();
        }));
    }
    refreshContextPanel();
}

void BattleRunSettingsPage::requestBattleContextForSeedProbe(qint64 seedProbeId)
{
    seedProbeId_ = seedProbeId;
    contextState_ = ContextState::ResolvingSeedProbe;
    if (!savestateForSeedProbeWatcher_.isRunning()) {
        savestateForSeedProbeWatcher_.setFuture(QtConcurrent::run([seedProbeId]() {
            return DataService::GetSavestateForSeedProbeAsync(seedProbeId).get();
        }));
    }
    refreshContextPanel();
}

void BattleRunSettingsPage::requestFreshBattleContext()
{
    if (savestateId_ <= 0 || freshContextJobWatcher_.isRunning()) {
        return;
    }
    simcore::db::battle::ctx::BlueprintIni blueprint{};
    blueprint.savestate_id = savestateId_;
    blueprint.priority = 100;
    blueprint.run_ms = 20000;
    blueprint.vi_stall_ms = 20000;
    freshContextJobWatcher_.setFuture(QtConcurrent::run([blueprint]() {
        return DataService::GetNewBattleContextAsync(blueprint.to_string()).get();
    }));
}

void BattleRunSettingsPage::pollBattleContextJob()
{
    if (contextJobId_ <= 0 || contextPollWatcher_.isRunning()) {
        return;
    }
    contextPollWatcher_.setFuture(QtConcurrent::run([jobId = contextJobId_]() {
        return JobsRepo::GetAsync(jobId).get();
    }));
}

void BattleRunSettingsPage::handleBattleContextLoaded(const BattleContextRow& row)
{
    contextCodecVersion_ = static_cast<int>(row.codec_version);
    if (!contextDecodeWatcher_.isRunning()) {
        contextDecodeWatcher_.setFuture(QtConcurrent::run([artifactId = row.artifact_id]() {
            return ObjectStore::GetTextAsync(artifactId).get();
        }));
    }
}

void BattleRunSettingsPage::clearBattleContext()
{
    hasContext_ = false;
    battleContext_ = {};
    contextState_ = ContextState::None;
    contextCodecVersion_ = 0;
    seedProbeId_ = 0;
    savestateId_ = 0;
    contextJobId_ = 0;
    contextPollTimer_.stop();
    invalidCells_.clear();
    invalidReasons_.clear();
    estimateState_ = {};
    refreshAllViews();
}

void BattleRunSettingsPage::openAddPresetDialog(std::optional<std::pair<int, int>> targetCell, std::optional<qint64> initialPresetId)
{
    PresetEditorDialog dialog(hasContext_ ? &battleContext_ : nullptr, this);
    if (initialPresetId.has_value() && initialPresetId.value() > 0) {
        const auto result = TurnActionPresetRepo::Get(initialPresetId.value());
        if (result.ok) {
            dialog.loadRow(result.value);
        }
    }
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    bool ok = false;
    QString errorText;
    TurnActionPresetRow row = dialog.buildRow(&ok, &errorText);
    if (!ok) {
        setErrorMessage(errorText);
        refreshInlineMessage();
        return;
    }

    DbResult<qint64> persistResult;
    if (row.id > 0) {
        const DbResult<void> update = TurnActionPresetRepo::Update(row);
        if (update.ok) {
            persistResult.ok = true;
            persistResult.value = row.id;
        } else {
            persistResult.ok = false;
            persistResult.error = update.error;
        }
    } else {
        persistResult = TurnActionPresetRepo::Insert(row);
    }

    if (!persistResult.ok) {
        setErrorMessage(QStringLiteral("Failed to save UI action preset: %1").arg(QString::fromStdString(persistResult.error.message)));
        refreshInlineMessage();
        return;
    }

    const qint64 presetId = persistResult.value;
    const auto reload = TurnActionPresetRepo::Get(presetId);
    if (reload.ok) {
        presetCache_.insert(presetId, reload.value);
    }

    if (targetCell.has_value()) {
        assignPresetToCell(targetCell->first, targetCell->second, presetId);
    }
    refreshUiActionLibrary();
}

void BattleRunSettingsPage::openAddPredicateDialog(std::optional<int> editIndex, std::optional<qint64> initialPredicateId)
{
    PredicateEditorDialog dialog(this);
    if (initialPredicateId.has_value() && initialPredicateId.value() > 0) {
        const auto result = PredicateSpecRepo::Get(initialPredicateId.value());
        if (result.ok) {
            dialog.loadRow(result.value);
        }
    }
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    bool ok = false;
    QString errorText;
    PredicateSpecRow row = dialog.buildRow(&ok, &errorText);
    if (!ok) {
        setErrorMessage(errorText);
        refreshInlineMessage();
        return;
    }

    if (!dialog.lhsProgramBlob().isEmpty()) {
        const QByteArray blob = dialog.lhsProgramBlob();
        const auto lhsProgramResult = simcore::db::AddressProgramRepo::Ensure(
            static_cast<int32_t>(addrprog::PROG_VERSION),
            std::vector<uint8_t>(blob.begin(), blob.end()),
            std::nullopt,
            std::nullopt,
            std::nullopt,
            dialog.lhsProgramDescription().isEmpty() ? std::string("lhs program") : dialog.lhsProgramDescription().toStdString());
        if (!lhsProgramResult.ok) {
            setErrorMessage(QStringLiteral("Failed to save LHS address program: %1").arg(QString::fromStdString(lhsProgramResult.error.message)));
            refreshInlineMessage();
            return;
        }
        row.lhs_prog_id = lhsProgramResult.value;
    }

    if (!dialog.rhsProgramBlob().isEmpty()) {
        const QByteArray blob = dialog.rhsProgramBlob();
        const auto rhsProgramResult = simcore::db::AddressProgramRepo::Ensure(
            static_cast<int32_t>(addrprog::PROG_VERSION),
            std::vector<uint8_t>(blob.begin(), blob.end()),
            std::nullopt,
            std::nullopt,
            std::nullopt,
            dialog.rhsProgramDescription().isEmpty() ? std::string("rhs program") : dialog.rhsProgramDescription().toStdString());
        if (!rhsProgramResult.ok) {
            setErrorMessage(QStringLiteral("Failed to save RHS address program: %1").arg(QString::fromStdString(rhsProgramResult.error.message)));
            refreshInlineMessage();
            return;
        }
        row.rhs_prog_id = rhsProgramResult.value;
    }

    DbResult<qint64> persistResult;
    if (row.id > 0) {
        const DbResult<int64_t> ensured = PredicateSpecRepo::EnsureByFingerprint(row);
        persistResult = ensured;
    } else {
        persistResult = PredicateSpecRepo::EnsureByFingerprint(row);
    }

    if (!persistResult.ok) {
        setErrorMessage(QStringLiteral("Failed to save predicate: %1").arg(QString::fromStdString(persistResult.error.message)));
        refreshInlineMessage();
        return;
    }

    const qint64 predicateId = persistResult.value;
    const auto fullRow = PredicateSpecRepo::Get(predicateId);
    if (fullRow.ok) {
        predicateRowCache_.insert(predicateId, fullRow.value);
        PredicateDraft draft{ predicateId, QString::fromStdString(fullRow.value.name), QString::fromStdString(fullRow.value.description) };
        if (editIndex.has_value() && editIndex.value() >= 0 && editIndex.value() < predicates_.size()) {
            predicates_[editIndex.value()] = draft;
        } else {
            predicates_.push_back(draft);
        }
        refreshPredicateDraftView();
    }
    refreshPredicateLibrary();
}

void BattleRunSettingsPage::openSaveTemplateDialog()
{
    TemplateSaveDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (dialog.name().isEmpty()) {
        setErrorMessage(QStringLiteral("Template name is required."));
        refreshInlineMessage();
        return;
    }
    saveAuthoringTemplate(dialog.name(), dialog.description());
}

void BattleRunSettingsPage::loadAuthoringTemplate(qint64 templateId)
{
    const auto result = AuthoringTemplatesRepo::Get(templateId);
    if (!result.ok) {
        setErrorMessage(QStringLiteral("Failed to load authoring template: %1").arg(QString::fromStdString(result.error.message)));
        refreshInlineMessage();
        return;
    }

    const AuthoringTemplateRow& row = result.value;
    const IniDoc uiIni = IniDoc::parse(row.ui_config_ini);
    const IniDoc predIni = IniDoc::parse(row.predicate_specs_ini);

    std::vector<int64_t> uiIds;
    for (const std::string& idString : uiIni.get_list(kUiConfigSectionName, "ids")) {
        uiIds.push_back(std::stoll(idString));
    }
    std::vector<int64_t> predicateIds;
    for (const std::string& idString : predIni.get_list(kPredicateSectionName, "ids")) {
        predicateIds.push_back(std::stoll(idString));
    }

    uiConfig_.actions.clear();

    const auto uiRowsResult = DataService::GetUiConfigRowsByIdsAsync(uiIds).get();
    if (uiRowsResult.ok) {
        for (const UiConfigRow& uiRow : uiRowsResult.value) {
            while (uiConfig_.actions.size() <= uiRow.turn_index) {
                addTurn();
            }
            while (uiConfig_.actions[uiRow.turn_index].size() <= uiRow.actor_slot) {
                uiConfig_.actions[uiRow.turn_index].push_back({ static_cast<quint32>(uiConfig_.actions[uiRow.turn_index].size()), 0 });
            }
            uiConfig_.actions[uiRow.turn_index][uiRow.actor_slot].actorSlot = static_cast<quint32>(uiRow.actor_slot);
            uiConfig_.actions[uiRow.turn_index][uiRow.actor_slot].presetId = uiRow.preset_id;
        }
    }

    predicates_.clear();
    for (const qint64 predicateId : predicateIds) {
        const auto predResult = PredicateSpecRepo::Get(predicateId);
        if (predResult.ok) {
            predicateRowCache_.insert(predicateId, predResult.value);
            predicates_.push_back({ predicateId, QString::fromStdString(predResult.value.name), QString::fromStdString(predResult.value.description) });
        }
    }

    validateGridAgainstContext();
    computeEstimate();
    refreshAllViews();
    setInfoMessage(QStringLiteral("Loaded authoring template #%1.").arg(templateId));
}

bool BattleRunSettingsPage::buildUiConfigFromDraft(simcore::battleexplorer::UI_Config& cfg, QString* errorMessage) const
{
    cfg.turns.clear();
    cfg.initial_frames.clear();

    for (const QVector<UiActionInstance>& turnDraft : uiConfig_.actions) {
        simcore::battleexplorer::UI_Turn turn;
        for (const UiActionInstance& instance : turnDraft) {
            if (instance.presetId <= 0) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Every actor slot must have a preset assigned.");
                }
                return false;
            }
            TurnActionPresetRow preset;
            if (presetCache_.contains(instance.presetId)) {
                preset = presetCache_.value(instance.presetId);
            } else {
                const auto result = TurnActionPresetRepo::Get(instance.presetId);
                if (!result.ok) {
                    if (errorMessage) {
                        *errorMessage = QStringLiteral("Failed to resolve preset %1.").arg(instance.presetId);
                    }
                    return false;
                }
                preset = result.value;
            }

            simcore::battleexplorer::UI_Action action{};
            action.actor_slot = static_cast<uint8_t>(instance.actorSlot);
            action.macro = static_cast<BattleAction>(preset.macro);
            if (action.macro == BattleAction::UseItem) {
                action.params.item_id = static_cast<uint16_t>(preset.item_id);
            }

            simcore::battleexplorer::TargetBinding binding{};
            binding.kind = static_cast<TargetBindingKind>(preset.target_kind);
            binding.mask = 0;
            binding.var_id = 0xFF;

            bool handledExpression = false;
            if (!preset.target_expr_ini.empty()) {
                const IniDoc ini = IniDoc::parse(preset.target_expr_ini);
                if (ini.get("target", "kind", "") == "ByEnemyKind") {
                    const int enemyKindId = static_cast<int>(ini.get_i64("target", "enemy_kind_id", -1));
                    const std::string quantifier = ini.get("target", "quantifier", "Any");
                    quint32 domainMask = 0;
                    for (int slot = 4; slot <= 11; ++slot) {
                        if (battleContext_.slots_[slot].present == 1 && static_cast<int>(battleContext_.slots_[slot].id) == enemyKindId) {
                            domainMask |= (1u << slot);
                        }
                    }
                    if (domainMask == 0) {
                        if (errorMessage) {
                            *errorMessage = QStringLiteral("A symbolic target could not be resolved for the current BattleContext.");
                        }
                        return false;
                    }
                    if (quantifier == "First") {
                        int firstSlot = -1;
                        for (int slot = 4; slot <= 11 && firstSlot < 0; ++slot) {
                            if ((domainMask & (1u << slot)) != 0) {
                                firstSlot = slot;
                            }
                        }
                        binding.kind = TargetBindingKind::SingleEnemy;
                        binding.mask = (1u << firstSlot);
                    } else {
                        binding.kind = TargetBindingKind::MultipleEnemies;
                        binding.mask = domainMask;
                    }
                    handledExpression = true;
                }
            }

            if (!handledExpression) {
                switch (static_cast<TargetBindingKind>(preset.target_kind)) {
                case TargetBindingKind::SingleEnemy:
                    binding.mask = (1u << static_cast<quint32>(preset.single_slot));
                    break;
                case TargetBindingKind::MultipleEnemies:
                    binding.mask = static_cast<quint32>(preset.mask_bits);
                    break;
                case TargetBindingKind::AnyEnemy:
                    break;
                case TargetBindingKind::SameAsOtherPC:
                    binding.var_id = static_cast<uint8_t>(preset.same_as_pc);
                    break;
                }
            }

            action.target = binding;
            turn.push_back(action);
        }
        cfg.turns.push_back(turn);
    }

    cfg.initial_frames.push_back(GCInputFrame());
    return true;
}

void BattleRunSettingsPage::computeEstimate()
{
    estimateState_ = {};
    if (!hasContext_) {
        estimateState_.message = QStringLiteral("Load a BattleContext to estimate paths.");
        return;
    }
    simcore::battleexplorer::UI_Config cfg{};
    QString errorText;
    if (!buildUiConfigFromDraft(cfg, &errorText)) {
        estimateState_.message = errorText.isEmpty() ? QStringLiteral("Draft could not be converted into a UI config.") : errorText;
        return;
    }
    BattleExplorer explorer{ "" };
    estimateState_.basePlans = explorer.estimate_paths_no_fake(cfg, battleContext_);
    estimateState_.available = true;
}

void BattleRunSettingsPage::saveExplorerSettings()
{
    simcore::battleexplorer::UI_Config cfg{};
    QString errorText;
    if (!buildUiConfigFromDraft(cfg, &errorText)) {
        setErrorMessage(errorText);
        refreshInlineMessage();
        return;
    }

    std::vector<PredicateSpecRow> predicateRows;
    predicateRows.reserve(predicates_.size());
    for (const PredicateDraft& predicate : predicates_) {
        auto result = PredicateSpecRepo::Get(predicate.predicateId);
        if (!result.ok) {
            setErrorMessage(QStringLiteral("Failed to reload predicate %1.").arg(predicate.predicateId));
            refreshInlineMessage();
            return;
        }
        predicateRows.push_back(result.value);
    }

    const qint64 savestateId = savestateId_;
    const QString settingsName = settingsNameEdit_->text().trimmed();
    const QString settingsDescription = settingsDescriptionEdit_->toPlainText().trimmed();
    const soa::battle::ctx::BattleContext context = battleContext_;

    saveSettingsWatcher_.setFuture(QtConcurrent::run([savestateId, cfg, predicateRows, context, settingsName, settingsDescription]() mutable {
        simcore::phases::AuthoringPayload payload{};
        payload.ui = cfg;
        payload.predicates = predicateRows;
        payload.settings_name = settingsName.toStdString();
        payload.settings_description = settingsDescription.toStdString();
        return simcore::phases::BRSettingsWriter::EnsureSettingsWithPredicatesAndPlans(savestateId, context, payload);
    }));
}

void BattleRunSettingsPage::saveAuthoringTemplate(const QString& name, const QString& description)
{
    const UiConfigDraft draft = uiConfig_;
    const QVector<PredicateDraft> predicateDrafts = predicates_;
    saveTemplateWatcher_.setFuture(QtConcurrent::run([draft, predicateDrafts, name, description]() {
        std::vector<UiConfigRow> rows;
        for (int turnIndex = 0; turnIndex < draft.actions.size(); ++turnIndex) {
            for (const UiActionInstance& action : draft.actions.at(turnIndex)) {
                UiConfigRow row{};
                row.turn_index = turnIndex;
                row.actor_slot = static_cast<int>(action.actorSlot);
                row.preset_id = action.presetId;
                rows.push_back(row);
            }
        }

        const auto insertedRows = DataService::InsertUiConfigRowsAsync(rows).get();
        if (!insertedRows.ok) {
            DbResult<qint64> error;
            error.ok = false;
            error.error = insertedRows.error;
            return error;
        }

        IniDoc uiIni;
        uiIni.ensure_section(kUiConfigSectionName);
        std::vector<std::string> uiIds;
        uiIds.reserve(insertedRows.value.size());
        for (const int64_t rowId : insertedRows.value) {
            uiIds.push_back(std::to_string(rowId));
        }
        uiIni.set_list(kUiConfigSectionName, "ids", uiIds);

        IniDoc predicateIni;
        predicateIni.ensure_section(kPredicateSectionName);
        std::vector<std::string> predicateIds;
        predicateIds.reserve(predicateDrafts.size());
        for (const PredicateDraft& predicate : predicateDrafts) {
            predicateIds.push_back(std::to_string(predicate.predicateId));
        }
        predicateIni.set_list(kPredicateSectionName, "ids", predicateIds);

        AuthoringTemplateRow templateRow{};
        templateRow.name = name.toStdString();
        templateRow.description = description.toStdString();
        templateRow.ui_config_ini = uiIni.to_string_sorted();
        templateRow.predicate_specs_ini = predicateIni.to_string_sorted();
        return AuthoringTemplatesRepo::Insert(templateRow);
    }));
}

QString BattleRunSettingsPage::presetSummary(qint64 presetId) const
{
    if (presetId <= 0) {
        return QStringLiteral("[empty]");
    }
    if (presetCache_.contains(presetId)) {
        const TurnActionPresetRow preset = presetCache_.value(presetId);
        QString summary = QStringLiteral("[%1] %2 · %3 · %4")
            .arg(preset.id)
            .arg(QString::fromStdString(preset.name))
            .arg(actionName(preset.macro))
            .arg(targetKindName(preset.target_kind));
        if (preset.macro == static_cast<int>(BattleAction::UseItem)) {
            summary.append(QStringLiteral(" · %1").arg(itemName(preset.item_id)));
        }
        return summary;
    }
    return QStringLiteral("[%1] (loading...)").arg(presetId);
}

QString BattleRunSettingsPage::contextStateText() const
{
    switch (contextState_) {
    case ContextState::None:
        return QStringLiteral("No BattleContext loaded.");
    case ContextState::LoadingFromSavestate:
        return QStringLiteral("Loading BattleContext for savestate %1…").arg(savestateId_);
    case ContextState::ResolvingSeedProbe:
        return QStringLiteral("Resolving savestate for seed probe %1…").arg(seedProbeId_);
    case ContextState::Queued:
        return QStringLiteral("BattleContext request queued (job %1).").arg(contextJobId_);
    case ContextState::Running:
        return QStringLiteral("BattleContext request running (job %1).").arg(contextJobId_);
    case ContextState::Success:
        return QStringLiteral("BattleContext job succeeded. Reloading latest context…");
    case ContextState::Failure:
        return QStringLiteral("BattleContext request failed.");
    case ContextState::Decoded:
        return QStringLiteral("BattleContext loaded for savestate %1 (codec v%2).").arg(savestateId_).arg(contextCodecVersion_);
    }
    return QStringLiteral("BattleContext state unknown.");
}

QString BattleRunSettingsPage::describeContext() const
{
    QStringList lines;
    lines << QStringLiteral("Seed Probe ID: %1").arg(seedProbeId_ > 0 ? QString::number(seedProbeId_) : QStringLiteral("(none)"));
    lines << QStringLiteral("Savestate ID: %1").arg(savestateId_ > 0 ? QString::number(savestateId_) : QStringLiteral("(none)"));
    lines << QStringLiteral("Party size: %1").arg(partySize_);
    if (!hasContext_) {
        lines << QStringLiteral("No decoded BattleContext available.");
        return lines.join('\n');
    }
    for (int slot = 0; slot < soa::battle::ctx::SLOT_COUNT; ++slot) {
        const auto& entry = battleContext_.slots_[slot];
        if (entry.present != 1) {
            continue;
        }
        lines << QStringLiteral("Slot %1 · id=%2 · player=%3 · alive=%4")
            .arg(slot)
            .arg(entry.id)
            .arg(entry.is_player ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(entry.is_alive ? QStringLiteral("yes") : QStringLiteral("no"));
    }
    return lines.join('\n');
}

QString BattleRunSettingsPage::invalidReason(int turnIndex, int actorSlot) const
{
    return invalidReasons_.value(invalidCellKey(turnIndex, actorSlot));
}

void BattleRunSettingsPage::setInfoMessage(const QString& text)
{
    infoMessage_ = text;
    errorMessage_.clear();
    refreshInlineMessage();
}

void BattleRunSettingsPage::setErrorMessage(const QString& text)
{
    errorMessage_ = text;
    infoMessage_.clear();
    refreshInlineMessage();
}
