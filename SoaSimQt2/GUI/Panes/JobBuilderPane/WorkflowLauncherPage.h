#pragma once

#include <optional>
#include <vector>

#include <QtWidgets/QWidget>

#include "Authoring/IAuthoringDb.h"
#include "GUI/Common/StatusToast.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QCheckBox;
class QFrame;
class QSpinBox;
class QTableWidget;

class WorkflowLauncherPage final : public QWidget
{
    Q_OBJECT

public:
    explicit WorkflowLauncherPage(QWidget* parent = nullptr);

signals:
    void statusToastRequested(StatusToast toast);

private:
    struct ExternalInputRow {
        QString node_key;
        QString node_name;
        QString input_key;
        QString display_name;
        QString data_kind;
        QString default_ref_kind;
    };

    void createWidgets();
    void refreshWorkflowGraphs();
    void handleGraphSelectionChanged();
    void launchSelectedGraph();
    void populateExternalInputs(const simcore::db::WorkflowGraphSnapshot& graph);
    std::optional<simcore::db::WorkflowGraphSnapshot> selectedGraph() const;
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    static QString graphListText(const simcore::db::WorkflowGraphSnapshot& graph);
    static QString nodeDisplayName(const simcore::db::WorkflowGraphSnapshot& graph, const std::string& node_key);
    static QString defaultRefKindForDataKind(const QString& data_kind);
    static std::vector<QString> tasMovieNodeKeys(const simcore::db::WorkflowGraphSnapshot& graph);
    static std::vector<QString> battleChainNodeKeys(const simcore::db::WorkflowGraphSnapshot& graph);

    QListWidget* graphList_ = nullptr;
    QLineEdit* rootScopeKindEdit_ = nullptr;
    QLineEdit* rootScopeIdEdit_ = nullptr;
    QLabel* rtcRangeLabel_ = nullptr;
    QFrame* rtcRangePanel_ = nullptr;
    QLineEdit* rtcLowEdit_ = nullptr;
    QLineEdit* rtcHighEdit_ = nullptr;
    QCheckBox* battleFakeOverrideCheck_ = nullptr;
    QLabel* battleFakeRangeLabel_ = nullptr;
    QFrame* battleFakeRangePanel_ = nullptr;
    QSpinBox* battleFakeMinSpin_ = nullptr;
    QSpinBox* battleFakeMaxSpin_ = nullptr;
    QTableWidget* externalInputsTable_ = nullptr;
    QLabel* graphDetailLabel_ = nullptr;
    QLabel* launchStatusLabel_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* launchButton_ = nullptr;

    std::vector<simcore::db::WorkflowGraphSnapshot> workflowGraphs_;
    std::vector<ExternalInputRow> externalInputs_;
};
