#pragma once

#include <QtCore/QFutureWatcher>
#include <QtCore/QSet>
#include <QtWidgets/QWidget>

#include "DB/SavorDbArchiveService.h"
#include "GUI/Common/StatusToast.h"
#include "GUI/Refresh/DatabaseProjectionController.h"

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
class QTabWidget;
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

    struct RehydratePackageRefreshRequest {
        savorqt::db::ArchivePackageFilter filter;
    };

    struct RehydratePackageRefreshData {
        savorqt::db::ArchivePackagePage packages;
    };

    struct RehydratePreviewRefreshRequest {
        std::int64_t archive_package_id = 0;
        std::string target_namespace;
    };

    struct RehydratePreviewRefreshData {
        savorqt::db::ArchiveRehydratePreviewResult preview;
    };

    struct RehydrateRequestRefreshRequest {
        std::int64_t archive_package_id = 0;
    };

    struct RehydrateRequestRefreshData {
        savorqt::db::ArchiveRehydrateRequestPage requests;
    };

    using CandidateResult = savorqt::db::ServiceResult<CandidateRefreshData>;
    using PreviewResult = savorqt::db::ServiceResult<PreviewRefreshData>;
    using ExecuteResult = savorqt::db::ServiceResult<savorqt::db::ArchiveWorkflowExecuteResult>;
    using RehydratePackageResult = savorqt::db::ServiceResult<RehydratePackageRefreshData>;
    using RehydratePreviewResult = savorqt::db::ServiceResult<RehydratePreviewRefreshData>;
    using RehydrateRequestResult = savorqt::db::ServiceResult<RehydrateRequestRefreshData>;
    using RehydrateExecuteResult = savorqt::db::ServiceResult<savorqt::db::ArchiveRehydrateExecuteResult>;
    using RehydrateCleanupResult = savorqt::db::ServiceResult<savorqt::db::ArchiveRehydrateCleanupResult>;

    void createWidgets();
    void wireSignals();
    void createRehydrateWidgets(QWidget* tab);
    void refreshCandidates();
    void requestPreview();
    void executeArchive();
    void refreshArchivePackages();
    void refreshRehydrateRequests();
    void requestRehydratePreview();
    void executeRehydrate();
    void cleanupSelectedRehydrateRequest();
    void applyCandidateRows(const std::vector<savorqt::db::ArchiveCandidateRow>& rows);
    void applyArchivePackageRows(const std::vector<savor::db::UiArchiveCatalogRow>& rows);
    void applyRehydrateRequestRows(const std::vector<savor::db::UiArchiveRehydrateRequestRow>& rows);
    void updatePreviewLabels();
    void updateExecuteState();
    void updateRehydratePreviewLabels();
    void updateRehydrateExecuteState();
    void updateInlineMessage();
    void setBusy(bool busy);
    void setRehydrateBusy(bool busy);
    void resetArchiveProgress();
    void resetRehydrateProgress();
    void applyArchiveProgress(const savor::db::archive::ArchiveOperationProgress& progress);
    void applyRehydrateProgress(const savor::db::archive::ArchiveOperationProgress& progress);
    QString fallbackArchiveName() const;
    QString fallbackRehydrateNamespace() const;
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    savorqt::db::ArchiveWorkflowFilter currentFilter() const;
    savorqt::db::ArchiveSelectionBuildRequest currentSelectionRequest() const;
    savorqt::db::ArchivePackageFilter currentArchivePackageFilter() const;
    std::optional<savor::db::UiArchiveCatalogRow> selectedArchivePackage() const;
    std::optional<savor::db::UiArchiveRehydrateRequestRow> selectedRehydrateRequest() const;
    std::vector<std::int64_t> visibleWorkflowIds() const;
    std::vector<std::int64_t> selectedHighlightedWorkflowIds() const;
    void setVisibleRowsSelected(bool selected);
    void markHighlightedRowsExcluded();
    void clearSelectionState();

    bool pageActive_ = false;
    bool candidateFetchInFlight_ = false;
    bool previewFetchInFlight_ = false;
    bool executeInFlight_ = false;
    bool rehydratePackageFetchInFlight_ = false;
    bool rehydratePreviewFetchInFlight_ = false;
    bool rehydrateRequestFetchInFlight_ = false;
    bool rehydrateExecuteInFlight_ = false;
    bool rehydrateCleanupInFlight_ = false;
    bool includeFilterMatches_ = false;
    std::int64_t selectedArchivePackageId_ = 0;
    std::set<std::int64_t> explicitIncludes_;
    std::set<std::int64_t> explicitExclusions_;
    std::vector<CandidateRow> currentRows_;
    std::vector<savor::db::UiArchiveCatalogRow> currentArchivePackages_;
    std::vector<savor::db::UiArchiveRehydrateRequestRow> currentRehydrateRequests_;
    std::optional<PreviewRefreshData> currentPreview_;
    std::optional<RehydratePreviewRefreshData> currentRehydratePreview_;
    QString inlineMessage_;
    QString rehydrateInlineMessage_;

    savorqt::gui::DatabaseProjectionController<CandidateRefreshRequest, CandidateResult>* candidateRefreshPipeline_ = nullptr;
    savorqt::gui::DatabaseProjectionController<PreviewRefreshRequest, PreviewResult>* previewRefreshPipeline_ = nullptr;
    savorqt::gui::DatabaseProjectionController<RehydratePackageRefreshRequest, RehydratePackageResult>* rehydratePackageRefreshPipeline_ = nullptr;
    savorqt::gui::DatabaseProjectionController<RehydratePreviewRefreshRequest, RehydratePreviewResult>* rehydratePreviewRefreshPipeline_ = nullptr;
    savorqt::gui::DatabaseProjectionController<RehydrateRequestRefreshRequest, RehydrateRequestResult>* rehydrateRequestRefreshPipeline_ = nullptr;
    QFutureWatcher<ExecuteResult> executeWatcher_;
    QFutureWatcher<RehydrateExecuteResult> rehydrateExecuteWatcher_;
    QFutureWatcher<RehydrateCleanupResult> rehydrateCleanupWatcher_;

    QTabWidget* tabWidget_ = nullptr;
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

    QLineEdit* rehydrateSearchEdit_ = nullptr;
    QComboBox* rehydrateScopeFilter_ = nullptr;
    QComboBox* rehydrateChecksumFilter_ = nullptr;
    QCheckBox* rehydrateWorkflowOnlyCheck_ = nullptr;
    QCheckBox* rehydrateCreatedFromEnabled_ = nullptr;
    QDateTimeEdit* rehydrateCreatedFromEdit_ = nullptr;
    QCheckBox* rehydrateCreatedToEnabled_ = nullptr;
    QDateTimeEdit* rehydrateCreatedToEdit_ = nullptr;
    QPushButton* rehydrateRefreshButton_ = nullptr;
    QLabel* rehydrateCatalogStatusLabel_ = nullptr;
    QTableWidget* rehydratePackageTable_ = nullptr;
    QLabel* rehydratePreviewLabel_ = nullptr;
    QLabel* rehydrateBlockersLabel_ = nullptr;
    QLineEdit* rehydrateNamespaceEdit_ = nullptr;
    QLineEdit* rehydrateConfirmationEdit_ = nullptr;
    QPushButton* rehydrateExecuteButton_ = nullptr;
    QProgressBar* rehydrateProgressBar_ = nullptr;
    QLabel* rehydrateProgressPhaseLabel_ = nullptr;
    QLabel* rehydrateStatusLabel_ = nullptr;
    QTableWidget* rehydrateRequestTable_ = nullptr;
    QPushButton* rehydrateCleanupButton_ = nullptr;
};
