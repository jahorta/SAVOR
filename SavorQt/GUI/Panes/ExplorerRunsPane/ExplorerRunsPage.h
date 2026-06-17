#pragma once

#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <cstdint>
#include <vector>

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
    struct GroupRow {
        qint64 jobSetId = 0;
        QString created;
        QString jobs;
        QString results;
        QString status;
    };

    struct WaveTreeRow {
        qint64 jobSetId = 0;
        QString label;
        QString jobs;
        QString status;
    };

    struct JobRow {
        qint64 jobId = 0;
        QString state;
    };

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
    bool updatePlainText(QTextEdit* edit, const QString& text);

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
    std::vector<GroupRow> currentGroupRows_;
    std::vector<WaveTreeRow> currentWaveRows_;
    std::vector<JobRow> currentJobRows_;
    bool refreshingSelection_ = false;
};
