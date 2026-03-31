#pragma once

#include <QtWidgets/QWidget>
#include "GUI/Common/StatusToast.h"

class QCheckBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTreeView;
class SeedProbeController;
class SeedProbeGridWidget;
class SeedProbeListModel;
class SeedProbeUniqueTableModel;

class SeedProbePage final : public QWidget
{
    Q_OBJECT

signals:
    void statusToastRequested(StatusToast toast);

public:
    explicit SeedProbePage(QWidget* parent = nullptr);

private:
    void createWidgets();
    void wireSignals();
    void syncControlsFromController();
    void refreshList();
    void refreshDetails();
    void updateStatusWidgets();
    void rebuildLegend(const QVector<int>& deltas);

    SeedProbeController* controller_ = nullptr;
    SeedProbeListModel* listModel_ = nullptr;
    SeedProbeUniqueTableModel* uniqueModel_ = nullptr;

    QLabel* titleLabel_ = nullptr;
    QLabel* descriptionLabel_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QCheckBox* doneOnlyCheck_ = nullptr;
    QSpinBox* pageSizeSpin_ = nullptr;
    QCheckBox* autoRefreshCheck_ = nullptr;
    QSpinBox* refreshSecondsSpin_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QPushButton* prevButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QLabel* pageSummaryLabel_ = nullptr;
    QLabel* lastRefreshLabel_ = nullptr;
    QLabel* inlineMessageLabel_ = nullptr;

    QTreeView* listTable_ = nullptr;
    QLabel* neutralSeedValue_ = nullptr;
    QLabel* probeIdValue_ = nullptr;
    QLabel* statusValue_ = nullptr;
    QLabel* codecValue_ = nullptr;
    QLabel* savestateValue_ = nullptr;
    SeedProbeGridWidget* mainGrid_ = nullptr;
    SeedProbeGridWidget* cStickGrid_ = nullptr;
    SeedProbeGridWidget* triggerGrid_ = nullptr;
    QHBoxLayout* legendLayout_ = nullptr;
    QTreeView* uniqueTable_ = nullptr;
    QString lastToastSignature_;
};
