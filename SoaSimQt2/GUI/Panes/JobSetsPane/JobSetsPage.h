#pragma once

#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtWidgets/QWidget>

#include <memory>

#include "GUI/Common/StatusToast.h"
#include "DB/Querying/JobSetListDTO.h"
#include "JobSetsProgressDelegate.h"

#include <optional>

class JobSetsController;
class JobSetsTreeModel;
class JobSetsTreeView;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;

class JobSetsPage final : public QWidget
{
    Q_OBJECT

public:
    explicit JobSetsPage(QWidget* parent = nullptr);
    void setPageActive(bool active);

signals:
    void statusToastRequested(StatusToast toast);

private:
    void createWidgets();
    void wireSignals();
    void syncControlsFromController();
    void refreshModel();
    void updateStatusWidgets();
    void updateLoadingIndicatorState();
    std::optional<int> selectedProgramKind() const;
    std::optional<JobSetStateFilter> selectedStateFilter() const;
    std::optional<QString> selectedTagKey() const;

    JobSetsController* controller_ = nullptr;
    JobSetsTreeModel* treeModel_ = nullptr;
    JobSetsTreeView* treeView_ = nullptr;

    QLabel* titleLabel_ = nullptr;
    QLabel* descriptionLabel_ = nullptr;
    QComboBox* kindFilter_ = nullptr;
    QComboBox* stateFilter_ = nullptr;
    QComboBox* tagFilter_ = nullptr;
    QSpinBox* pageSizeSpin_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QCheckBox* autoRefreshCheck_ = nullptr;
    QSpinBox* refreshSecondsSpin_ = nullptr;
    QPushButton* prevButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QLabel* pageSummaryLabel_ = nullptr;
    QLabel* pageStatusLabel_ = nullptr;
    QLabel* lastRefreshLabel_ = nullptr;
    QLabel* inlineMessageLabel_ = nullptr;

    QHash<int, QString> programNames_;
    std::unique_ptr<JobSetsProgressDelegate> progressDelegate_;
    QTimer* loadingStateTimer_ = nullptr;
    bool delayedLoadingVisible_ = false;
    QString lastToastSignature_;
};
