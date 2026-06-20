#pragma once

#include <QtCore/QFutureWatcher>
#include <QtCore/QSet>
#include <QtWidgets/QWidget>

#include "DB/SavorDbArchiveService.h"
#include "GUI/Common/StatusToast.h"
#include "GUI/Refresh/AsyncRefreshPipeline.h"

#include <cstdint>
#include <optional>
#include <set>
#include <vector>

class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTextEdit;

class ArchiveWorkbenchPage final : public QWidget
{
    Q_OBJECT

public:
    explicit ArchiveWorkbenchPage(QWidget* parent = nullptr);
    void setPageActive(bool active);

signals:
    void statusToastRequested(StatusToast toast);

private:
    struct CandidateRow {
        savorqt::db::ArchiveCandidateRow row;
        bool selected = false;
        bool excluded = false;
    };

    struct CandidateRefreshRequest {
        savorqt::db::ArchiveWorkflowFilter filter;
    };

    struct CandidateRefreshData {
        savorqt::db::ArchiveWorkflowFilter filter;
        savorqt::db::ArchiveCandidatePage page;
    };

    struct PreviewRefreshRequest {
        savorqt::db::ArchiveSelectionBuildRequest selection_request;
    };

    struct PreviewRefreshData {
        savorqt::db::ArchiveSelectionBuildResult selection;
        savorqt::db::ArchiveWorkflowPreviewResult preview;
    };

    using CandidateResult = savorqt::db::ServiceResult<CandidateRefreshData>;
    using PreviewResult = savorqt::db::ServiceResult<PreviewRefreshData>;
    using ExecuteResult = savorqt::db::ServiceResult<savorqt::db::ArchiveWorkflowExecuteResult>;

    void createWidgets();
    void wireSignals();
    void refreshCandidates();
    void requestPreview();
    void executeArchive();
    void applyCandidateRows(const std::vector<savorqt::db::ArchiveCandidateRow>& rows);
    void updatePreviewLabels();
    void updateExecuteState();
    void updateInlineMessage();
    void setBusy(bool busy);
    void resetArchiveProgress();
    void applyArchiveProgress(const savor::db::archive::ArchiveOperationProgress& progress);
    QString fallbackArchiveName() const;
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    savorqt::db::ArchiveWorkflowFilter currentFilter() const;
    savorqt::db::ArchiveSelectionBuildRequest currentSelectionRequest() const;
    std::vector<std::int64_t> visibleWorkflowIds() const;
    std::vector<std::int64_t> selectedHighlightedWorkflowIds() const;
    void setVisibleRowsSelected(bool selected);
    void markHighlightedRowsExcluded();
    void clearSelectionState();

    bool pageActive_ = false;
    bool candidateFetchInFlight_ = false;
    bool previewFetchInFlight_ = false;
    bool executeInFlight_ = false;
    bool includeFilterMatches_ = false;
    std::set<std::int64_t> explicitIncludes_;
    std::set<std::int64_t> explicitExclusions_;
    std::vector<CandidateRow> currentRows_;
    std::optional<PreviewRefreshData> currentPreview_;
    QString inlineMessage_;

    savorqt::gui::AsyncRefreshPipeline<CandidateRefreshRequest, CandidateResult>* candidateRefreshPipeline_ = nullptr;
    savorqt::gui::AsyncRefreshPipeline<PreviewRefreshRequest, PreviewResult>* previewRefreshPipeline_ = nullptr;
    QFutureWatcher<ExecuteResult> executeWatcher_;

    QComboBox* displayStateFilter_ = nullptr;
    QLineEdit* workflowKindFilter_ = nullptr;
    QComboBox* finalVictoryFilter_ = nullptr;
    QComboBox* problemFilter_ = nullptr;
    QLineEdit* textFilter_ = nullptr;
    QCheckBox* createdFromEnabled_ = nullptr;
    QDateTimeEdit* createdFromEdit_ = nullptr;
    QCheckBox* createdToEnabled_ = nullptr;
    QDateTimeEdit* createdToEdit_ = nullptr;
    QCheckBox* completedFromEnabled_ = nullptr;
    QDateTimeEdit* completedFromEdit_ = nullptr;
    QCheckBox* completedToEnabled_ = nullptr;
    QDateTimeEdit* completedToEdit_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* selectVisibleButton_ = nullptr;
    QPushButton* clearVisibleButton_ = nullptr;
    QPushButton* selectAllMatchingButton_ = nullptr;
    QPushButton* excludeSelectedButton_ = nullptr;
    QPushButton* clearExclusionsButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* previewLabel_ = nullptr;
    QLabel* blockersLabel_ = nullptr;
    QCheckBox* packageOnlyCheck_ = nullptr;
    QCheckBox* purgeAfterVerifyCheck_ = nullptr;
    QLineEdit* archiveNameEdit_ = nullptr;
    QTextEdit* archiveNotesEdit_ = nullptr;
    QLineEdit* confirmationEdit_ = nullptr;
    QPushButton* executeButton_ = nullptr;
    QProgressBar* archiveProgressBar_ = nullptr;
    QLabel* archiveProgressPhaseLabel_ = nullptr;
    QLabel* executeStatusLabel_ = nullptr;
    QTableWidget* candidateTable_ = nullptr;
};
