#pragma once

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QFutureWatcher>
#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QVector>
#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"
#include "Core/Memory/Soa/Battle/BattleContext.h"
#include "Phases/BattleExplorer.h"
#include "DB/AuthoringTemplatesRepo.h"
#include "DB/BattleContextRepo.h"
#include "DB/PredicateSpecRepo.h"
#include "DB/Querying/DataService.h"
#include "DB/Scheduling/JobsRepo.h"
#include "DB/TurnActionPresetRepo.h"

#include <optional>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QPlainTextEdit;
class ActionPresetDragTableModel;
class PredicateDragTableModel;
class SelectedPredicateDropListWidget;
class QStandardItemModel;
class QTreeView;
class QVBoxLayout;
class BattleContextTreeWidget;

class BattleRunSettingsPage final : public QWidget
{
    Q_OBJECT

public:
    explicit BattleRunSettingsPage(QWidget* parent = nullptr);
    ~BattleRunSettingsPage() override;

signals:
    void statusToastRequested(StatusToast toast);

private:
    struct UiActionInstance {
        quint32 actorSlot = 0;
        qint64 presetId = 0;
    };

    struct UiConfigDraft {
        QVector<QVector<UiActionInstance>> actions;
    };

    struct PredicateDraft {
        qint64 predicateId = 0;
        QString name;
        QString description;
    };

    enum class ContextState : quint8 {
        None,
        LoadingFromSavestate,
        ResolvingSeedProbe,
        Queued,
        Running,
        Success,
        Failure,
        Decoded
    };

    struct EstimateState {
        bool available = false;
        QString message;
        quint64 basePlans = 0;
    };

    using ActionListResult = simcore::db::DbResult<std::vector<simcore::db::TurnActionPresetLite>>;
    using PredicateListResult = simcore::db::DbResult<std::vector<simcore::db::PredicateSpecLite>>;
    using TemplateListResult = simcore::db::DbResult<std::vector<simcore::db::AuthoringTemplateLite>>;
    using ContextLookupResult = simcore::db::DbResult<std::optional<simcore::db::BattleContextRow>>;
    using TextResult = simcore::db::DbResult<std::string>;
    using SaveTemplateResult = simcore::db::DbResult<qint64>;
    using SaveSettingsResult = simcore::db::DbResult<qint64>;

    void createWidgets();
    void wireSignals();
    void loadInitialData();
    void refreshUiActionLibrary();
    void refreshPredicateLibrary();
    void refreshTemplateLibrary();
    void refreshAllViews();
    void refreshUiActionLibraryView();
    void refreshPredicateLibraryView();
    void refreshTemplateLibraryView();
    void refreshDraftViews();
    void refreshUiConfigEditor();
    void refreshPredicateDraftView();
    void refreshContextPanel();
    void refreshEstimatePanel();
    void refreshSavePanel();
    void refreshInlineMessage();
    void addTurn();
    void reconcilePartySize();
    void assignPresetToCell(int turnIndex, int actorSlot, qint64 presetId);
    void clearPresetAtCell(int turnIndex, int actorSlot);
    void validateGridAgainstContext();
    std::optional<PredicateDraft> makePredicateDraft(qint64 predicateId);
    bool addPredicateToDraft(qint64 predicateId, std::optional<int> insertRow = std::nullopt);
    bool allSlotsFilled() const;
    void openSeedProbePicker();
    void requestBattleContextForSavestate(qint64 savestateId);
    void requestBattleContextForSeedProbe(qint64 seedProbeId);
    void requestFreshBattleContext();
    void pollBattleContextJob();
    void handleBattleContextLoaded(const simcore::db::BattleContextRow& row);
    void clearBattleContext();
    void openAddPresetDialog(std::optional<std::pair<int, int>> targetCell = std::nullopt, std::optional<qint64> initialPresetId = std::nullopt);
    void openAddPredicateDialog(std::optional<int> editIndex = std::nullopt, std::optional<qint64> initialPredicateId = std::nullopt);
    void openSaveTemplateDialog();
    void loadAuthoringTemplate(qint64 templateId);
    bool buildUiConfigFromDraft(simcore::battleexplorer::UI_Config& cfg, QString* errorMessage = nullptr) const;
    void computeEstimate();
    void saveExplorerSettings();
    void saveAuthoringTemplate(const QString& name, const QString& description);
    QString presetSummary(qint64 presetId) const;
    QString contextStateText() const;
    QString describeContext() const;
    QString invalidReason(int turnIndex, int actorSlot) const;
    void setInfoMessage(const QString& text);
    void setErrorMessage(const QString& text);
    void postStatusToast(const QString& text, bool error, const QString& details = QString());

    QFutureWatcher<ActionListResult> actionListWatcher_;
    QFutureWatcher<PredicateListResult> predicateListWatcher_;
    QFutureWatcher<TemplateListResult> templateListWatcher_;
    QFutureWatcher<ContextLookupResult> contextLookupWatcher_;
    QFutureWatcher<TextResult> contextDecodeWatcher_;
    QFutureWatcher<simcore::db::DbResult<qint64>> savestateForSeedProbeWatcher_;
    QFutureWatcher<simcore::db::DbResult<qint64>> freshContextJobWatcher_;
    QFutureWatcher<simcore::db::DbResult<simcore::db::JobRow>> contextPollWatcher_;
    QFutureWatcher<SaveTemplateResult> saveTemplateWatcher_;
    QFutureWatcher<SaveSettingsResult> saveSettingsWatcher_;

    QVector<simcore::db::TurnActionPresetLite> uiActionResults_;
    QVector<simcore::db::PredicateSpecLite> predicateResults_;
    QVector<simcore::db::AuthoringTemplateLite> templateResults_;
    UiConfigDraft uiConfig_;
    QVector<PredicateDraft> predicates_;
    QHash<qint64, simcore::db::TurnActionPresetRow> presetCache_;
    QHash<qint64, simcore::db::PredicateSpecLite> predicateLiteCache_;
    QHash<qint64, simcore::db::PredicateSpecRow> predicateRowCache_;
    QHash<quint32, QString> invalidReasons_;
    QSet<quint32> invalidCells_;

    qint64 seedProbeId_ = 0;
    qint64 savestateId_ = 0;
    qint64 contextJobId_ = 0;
    bool hasContext_ = false;
    int contextCodecVersion_ = 0;
    int partySize_ = 4;
    ContextState contextState_ = ContextState::None;
    soa::battle::ctx::BattleContext battleContext_{};
    EstimateState estimateState_{};
    QString infoMessage_;
    QString errorMessage_;
    QTimer contextPollTimer_;

    QLineEdit* actionSearchEdit_ = nullptr;
    QPushButton* actionRefreshButton_ = nullptr;
    QPushButton* actionNewButton_ = nullptr;
    QTreeView* actionTable_ = nullptr;
    ActionPresetDragTableModel* actionTableModel_ = nullptr;

    QLineEdit* predicateSearchEdit_ = nullptr;
    QPushButton* predicateRefreshButton_ = nullptr;
    QPushButton* predicateNewButton_ = nullptr;
    QTreeView* predicateTable_ = nullptr;
    PredicateDragTableModel* predicateTableModel_ = nullptr;

    QLineEdit* templateSearchEdit_ = nullptr;
    QPushButton* templateRefreshButton_ = nullptr;
    QTreeView* templateTable_ = nullptr;
    QStandardItemModel* templateTableModel_ = nullptr;

    QString lastToastMessage_;
    std::optional<StatusToast::Severity> lastToastSeverity_;
    QPushButton* addTurnButton_ = nullptr;
    QWidget* uiConfigContainer_ = nullptr;
    QVBoxLayout* uiConfigLayout_ = nullptr;
    SelectedPredicateDropListWidget* predicateDraftList_ = nullptr;
    QPushButton* addPredicateButton_ = nullptr;
    QPushButton* movePredicateUpButton_ = nullptr;
    QPushButton* movePredicateDownButton_ = nullptr;
    QPushButton* removePredicateButton_ = nullptr;
    QPushButton* editPredicateButton_ = nullptr;

    QLabel* contextSummaryLabel_ = nullptr;
    BattleContextTreeWidget* contextTree_ = nullptr;
    QPushButton* pickSeedProbeButton_ = nullptr;
    QPushButton* clearContextButton_ = nullptr;
    QPushButton* getContextButton_ = nullptr;

    QLabel* countsLabel_ = nullptr;
    QLabel* estimateLabel_ = nullptr;

    QLineEdit* settingsNameEdit_ = nullptr;
    QPlainTextEdit* settingsDescriptionEdit_ = nullptr;
    QPushButton* saveSettingsButton_ = nullptr;
    QPushButton* saveTemplateButton_ = nullptr;
};
