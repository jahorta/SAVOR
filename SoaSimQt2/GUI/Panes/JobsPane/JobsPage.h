#pragma once

#include <QtWidgets/QWidget>

#include <optional>

#include "GUI/Common/StatusToast.h"

class QPoint;

class ArtifactsTableModel;
class ArtifactsTableView;
class JobsController;
class JobsTableModel;
class JobsTableView;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTextEdit;
class QTimer;

class JobsPage final : public QWidget
{
    Q_OBJECT

public:
    explicit JobsPage(QWidget* parent = nullptr);
    void setPageActive(bool active);

signals:
    void visualReplayRequested(qint64 jobId);
    void statusToastRequested(StatusToast toast);

private:
    void createWidgets();
    void wireSignals();
    void syncControlsFromController(bool syncAll = false);
    void refreshModel();
    void updateInspector();
    void updateStatusWidgets();
    void updateLoadingIndicatorState();
    QTextEdit* createReadOnlyTextEdit();
    std::optional<int> selectedProgramKind() const;
    std::optional<QString> selectedState() const;
    std::optional<qint64> selectedJobSetId() const;
    void handleRestartRequested();
    void showJobsContextMenu(const QPoint& position);

    JobsController* controller_ = nullptr;
    JobsTableModel* jobsModel_ = nullptr;
    ArtifactsTableModel* artifactsModel_ = nullptr;

    QLabel* titleLabel_ = nullptr;
    QLabel* descriptionLabel_ = nullptr;
    QComboBox* kindFilter_ = nullptr;
    QComboBox* stateFilter_ = nullptr;
    QLineEdit* jobSetFilter_ = nullptr;
    QSpinBox* pageSizeSpin_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QCheckBox* autoRefreshCheck_ = nullptr;
    QSpinBox* refreshSecondsSpin_ = nullptr;
    QPushButton* prevButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QLabel* pageSummaryLabel_ = nullptr;
    QLabel* lastRefreshLabel_ = nullptr;
    QLabel* pageStatusLabel_ = nullptr;
    QLabel* inlineMessageLabel_ = nullptr;
    QTimer* loadingStateTimer_ = nullptr;
    bool delayedLoadingVisible_ = false;
    QString lastToastSignature_;

    JobsTableView* jobsTable_ = nullptr;
    QLabel* inspectorSummary_ = nullptr;
    QTabWidget* inspectorTabs_ = nullptr;
    QLabel* overviewPriorityValue_ = nullptr;
    QLabel* overviewQueuedValue_ = nullptr;
    QLabel* overviewSelectionHint_ = nullptr;
    QTextEdit* eventsText_ = nullptr;
    QTextEdit* progressText_ = nullptr;
    QTextEdit* payloadText_ = nullptr;
    QPushButton* loadInputIniButton_ = nullptr;
    QTextEdit* resultsText_ = nullptr;
    ArtifactsTableView* artifactsTable_ = nullptr;

    bool refreshingModel_ = false;
    std::optional<qint64> pendingSelectedJobId_;

};
