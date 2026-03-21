#pragma once

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QFutureWatcher>
#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtWidgets/QWidget>

#include "DB/DeltaSeedRepo.h"
#include "DB/Querying/DataService.h"
#include "Phases/DBPhaseBuilder/PhaseBuilderPreview.h"
#include "Utils/IniDoc.h"

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QSplitter;
class QStackedWidget;
class QTreeWidget;

class JobBuilderPage final : public QWidget
{
public:
    explicit JobBuilderPage(QWidget* parent = nullptr);

private:
    struct ValidationEntry {
        QString field;
        QString message;
    };

    struct SubmitResultPayload {
        bool ok = false;
        QString errorMessage;
        qint64 firstJobSetId = 0;
        qint64 createdCount = 0;
    };

    void createWidgets();
    void wireSignals();
    void loadProgramKinds();
    void handleKindSelectionChanged();
    void loadDefaultsForSelectedKind();
    void syncWidgetsFromIni();
    void syncIniFromWidgets();
    void refreshValidation();
    void refreshPreviewPanel();
    void refreshSubmitPanel();
    void refreshIniPanel();
    void refreshSelectionLists();
    void refreshDeltaTree();
    void updateStatusMessage();
    void setInlineMessage(const QString& text, const QString& severity = QStringLiteral("info"));

    void openSavestatePicker();
    void openArtifactPicker();
    void openSeedProbePicker();
    void openExplorerSettingsPicker();

    void requestPreview();
    void requestSubmit();
    void enqueueDeltaLoad(qint64 probeId);

    int selectedProgramKind() const;
    QString selectedProgramKindName() const;
    bool canSubmit() const;
    QString joinLines(const QVector<QString>& lines) const;

    QFutureWatcher<simcore::db::DbResult<std::vector<simcore::db::ProgramKindKV>>> kindsWatcher_;
    QFutureWatcher<simcore::db::DbResult<simcore::db::phasebuilder::PhasePreview>> previewWatcher_;
    QFutureWatcher<SubmitResultPayload> submitWatcher_;

    std::vector<simcore::db::ProgramKindKV> programKinds_;
    IniDoc ini_;
    bool hasIni_ = false;
    QVector<ValidationEntry> validationEntries_;
    std::optional<simcore::db::phasebuilder::PhasePreview> preview_;
    QString previewErrorMessage_;
    QString submitErrorMessage_;
    QString submitInfoMessage_;
    bool previewBusy_ = false;
    bool submitBusy_ = false;

    qint64 savestateId_ = 0;
    qint64 artifactId_ = 0;
    qint64 primaryExplorerSettingsId_ = 0;
    qint64 primarySeedProbeId_ = 0;
    QVector<qint64> selectedExplorerSettingsIds_;
    QVector<qint64> selectedSeedProbeIds_;
    QHash<qint64, QVector<simcore::db::DeltaSeedRow>> deltaRowsByProbe_;
    QHash<qint64, QSet<qint64>> selectedDeltaIdsByProbe_;

    QLabel* titleLabel_ = nullptr;
    QLabel* descriptionLabel_ = nullptr;
    QLabel* inlineMessageLabel_ = nullptr;
    QComboBox* kindCombo_ = nullptr;
    QPushButton* resetDefaultsButton_ = nullptr;
    QPushButton* validateButton_ = nullptr;
    QPushButton* previewButton_ = nullptr;
    QSplitter* splitLayout_ = nullptr;
    QStackedWidget* formStack_ = nullptr;

    QWidget* seedProbeForm_ = nullptr;
    QLabel* savestateSummaryLabel_ = nullptr;
    QPushButton* pickSavestateButton_ = nullptr;
    QSpinBox* seedProbePrioritySpin_ = nullptr;
    QSpinBox* seedProbeRunMsSpin_ = nullptr;
    QSpinBox* seedProbeViMsSpin_ = nullptr;
    QCheckBox* seedProbeClearWinnersCheck_ = nullptr;
    QSpinBox* seedProbeSamplesSpin_ = nullptr;
    QSpinBox* seedProbeMinValueSpin_ = nullptr;
    QSpinBox* seedProbeMaxValueSpin_ = nullptr;
    QCheckBox* seedProbeCapTopCheck_ = nullptr;
    QCheckBox* seedProbeIgnoreTriggerCheck_ = nullptr;
    QSpinBox* seedProbeComboAttemptsSpin_ = nullptr;
    QSpinBox* seedProbeComboSamplerTriesSpin_ = nullptr;

    QWidget* tasMovieForm_ = nullptr;
    QLabel* artifactSummaryLabel_ = nullptr;
    QPushButton* pickArtifactButton_ = nullptr;
    QLineEdit* tasRtcLowEdit_ = nullptr;
    QLineEdit* tasRtcHighEdit_ = nullptr;
    QSpinBox* tasPrioritySpin_ = nullptr;
    QSpinBox* tasRunMsSpin_ = nullptr;
    QSpinBox* tasViMsSpin_ = nullptr;
    QSpinBox* tasHeadroomSpin_ = nullptr;
    QCheckBox* tasProgressEnableCheck_ = nullptr;
    QCheckBox* tasAutoQueueCheck_ = nullptr;

    QWidget* explorerForm_ = nullptr;
    QListWidget* explorerSettingsList_ = nullptr;
    QListWidget* explorerSeedProbesList_ = nullptr;
    QPushButton* addExplorerSettingsButton_ = nullptr;
    QPushButton* clearExplorerSettingsButton_ = nullptr;
    QPushButton* addExplorerSeedProbeButton_ = nullptr;
    QPushButton* clearExplorerSeedProbeButton_ = nullptr;
    QPushButton* useAllDeltasButton_ = nullptr;
    QPushButton* selectAllDeltasButton_ = nullptr;
    QTreeWidget* deltaTree_ = nullptr;
    QSpinBox* explorerPrioritySpin_ = nullptr;
    QSpinBox* explorerRunMsSpin_ = nullptr;
    QSpinBox* explorerViMsSpin_ = nullptr;
    QCheckBox* explorerProgressEnableCheck_ = nullptr;
    QCheckBox* explorerSingleTurnCheck_ = nullptr;
    QCheckBox* explorerAutoWaveTriggerCheck_ = nullptr;
    QSpinBox* explorerMinFakeAttacksSpin_ = nullptr;
    QSpinBox* explorerMaxFakeAttacksSpin_ = nullptr;

    QPlainTextEdit* validationText_ = nullptr;
    QPlainTextEdit* previewText_ = nullptr;
    QLabel* submitStatusLabel_ = nullptr;
    QLineEdit* purposeEdit_ = nullptr;
    QPlainTextEdit* metaEdit_ = nullptr;
    QPushButton* submitButton_ = nullptr;
    QPlainTextEdit* iniText_ = nullptr;
};
