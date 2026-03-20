#pragma once

#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtWidgets/QWidget>

#include "DB/Querying/JobSetListDTO.h"

#include <optional>

class JobSetsController;
class JobSetsProgressWidget;
class JobSetsTreeModel;
class JobSetsTreeView;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

class JobSetsPage final : public QWidget
{
public:
    explicit JobSetsPage(QWidget* parent = nullptr);

private:
    void createWidgets();
    void wireSignals();
    void syncControlsFromController();
    void refreshModel();
    void rebuildProgressWidgets();
    void updateStatusWidgets();
    std::optional<int> selectedProgramKind() const;
    std::optional<JobSetStateFilter> selectedStateFilter() const;
    void handleDeleteRequested(qint64 jobSetId);

    JobSetsController* controller_ = nullptr;
    JobSetsTreeModel* treeModel_ = nullptr;
    JobSetsTreeView* treeView_ = nullptr;

    QLabel* titleLabel_ = nullptr;
    QLabel* descriptionLabel_ = nullptr;
    QComboBox* kindFilter_ = nullptr;
    QComboBox* stateFilter_ = nullptr;
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
    QLabel* inlineMessageLabel_ = nullptr;

    QHash<int, QString> programNames_;
    QSet<qint64> expandedIds_;
};
