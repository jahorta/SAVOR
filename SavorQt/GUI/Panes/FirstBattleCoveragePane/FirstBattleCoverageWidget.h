#pragma once

#include "Execution/Workflow/WorkflowExpansionService.h"
#include "GUI/Common/StatusToast.h"

#include <QtWidgets/QWidget>

#include <cstdint>
#include <functional>
#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableView;
namespace savorqt::gui { template <typename Request, typename Result> class AsyncRefreshPipeline; }

struct FirstBattleCoverageRefreshRequest {
    std::int64_t source_dtm_artifact_id = 0;
    std::int64_t source_annotation_attempt_id = 0;
    std::int64_t source_root_establishment_attempt_id = 0;
    std::int64_t workflow_expansion_id = 0;
    std::int64_t rtc_min = 0;
    std::int64_t rtc_max = 0;
    std::int64_t max_neutral_epochs = 0;
};

struct FirstBattleCoverageRefreshData {
    bool ok = false;
    QString error;
    std::vector<savor::db::execution::workflow::PreparedTasRootSourceSnapshot> sources;
    savor::db::execution::workflow::FirstBattleCoverageSnapshot coverage;
};

class FirstBattleCoverageWidget final : public QWidget {
    Q_OBJECT
public:
    struct Actions {
        std::function<void(std::int64_t)> openWorkflow;
    };

    explicit FirstBattleCoverageWidget(Actions actions, QWidget* parent = nullptr);
    void setPageActive(bool active);
    void showExpansion(std::int64_t workflow_expansion_id);

signals:
    void statusToastRequested(StatusToast toast);

private:
    void requestRefresh();
    void applyRefresh(const FirstBattleCoverageRefreshData& data);
    void refreshSelectionDetails();
    void launchSelected();
    void retrySelected();
    void openSelectedWorkflow();

    Actions actions_;
    savorqt::gui::AsyncRefreshPipeline<FirstBattleCoverageRefreshRequest,
        FirstBattleCoverageRefreshData>* refresh_ = nullptr;
    QComboBox* source_ = nullptr;
    QSpinBox* rtcMin_ = nullptr;
    QSpinBox* rtcMax_ = nullptr;
    QSpinBox* maxDelay_ = nullptr;
    QTableView* table_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* details_ = nullptr;
    QLabel* selectionPreview_ = nullptr;
    QPushButton* runMissing_ = nullptr;
    QPushButton* retry_ = nullptr;
    QPushButton* openWorkflow_ = nullptr;
    std::int64_t focusExpansionId_ = 0;
    bool active_ = false;
    bool operationInFlight_ = false;
    savor::db::execution::workflow::FirstBattleCoverageSnapshot coverage_;
};
