#pragma once

#include <QtWidgets/QWidget>

#include <cstdint>

#include "GUI/Common/StatusToast.h"

class ExplorerRunsController;
class QLabel;
class QCheckBox;
class QSpinBox;
class QPushButton;
class QTableWidget;
class QTextEdit;
class QTreeWidget;
class QPoint;

class ExplorerRunsPage final : public QWidget
{
    Q_OBJECT

public:
    explicit ExplorerRunsPage(QWidget* parent = nullptr);
    ~ExplorerRunsPage() override = default;
    void setPageActive(bool active);

signals:
    void visualReplayRequested(qint64 jobId);
    void statusToastRequested(StatusToast toast);

private:
    void createWidgets();
    void wireSignals();
    void syncControls();
    void refreshGroupsTable();
    void refreshWaveTree();
    void refreshJobsTable();
    void refreshDetail();
    void updateStatusWidgets();
    void showJobContextMenu(const QPoint& position);
    qint64 selectedJobId() const;
    bool isFinishedState(const QString& state) const;
    QString formatTimestamp(qint64 epochMillis) const;
    QString groupStatusText(std::int64_t completed, std::int64_t total, std::int64_t failed, std::int64_t canceled) const;

    ExplorerRunsController* controller_ = nullptr;
    QCheckBox* autoRefreshCheck_ = nullptr;
    QSpinBox* refreshSecondsSpin_ = nullptr;
    QSpinBox* pageSizeSpin_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* prevButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QCheckBox* childVictoryOnlyCheck_ = nullptr;
    QCheckBox* winnersOnlyCheck_ = nullptr;
    QCheckBox* showDuplicatesCheck_ = nullptr;
    QCheckBox* successOnlyCheck_ = nullptr;
    QPushButton* triggerNextWaveButton_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QLabel* lastRefreshLabel_ = nullptr;
    QLabel* inlineMessageLabel_ = nullptr;
    QTableWidget* groupsTable_ = nullptr;
    QTreeWidget* waveTree_ = nullptr;
    QTableWidget* jobsTable_ = nullptr;
    QTextEdit* blueprintText_ = nullptr;
    QTextEdit* progressText_ = nullptr;
    QTextEdit* resultsText_ = nullptr;
    QString lastToastSignature_;
    bool refreshingSelection_ = false;
};
