#pragma once

#include <QtWidgets/QWidget>

#include <cstdint>
#include <functional>
#include <vector>

#include "GUI/Refresh/ViewState.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
namespace savorqt::db { struct VictoryResultSummary; }
namespace savorqt::gui { template <typename Request, typename Result> class DatabaseProjectionController; }

class VictoryResultsWidget final : public QWidget {
public:
    struct Actions {
        std::function<void(std::int64_t)> recordVictory;
        std::function<void(std::int64_t)> openWorkflow;
        std::function<void(std::int64_t)> openJob;
        std::function<void()> openArtifacts;
    };
    explicit VictoryResultsWidget(Actions actions, QWidget* parent = nullptr);
    void showRoute(std::int64_t route_node_id);

private:
    void applyRows(const std::vector<savorqt::db::VictoryResultSummary>& rows);
    void rebuildTable();
    void showSelected();

    Actions actions_;
    savorqt::gui::DatabaseProjectionController<std::int64_t,
        std::vector<savorqt::db::VictoryResultSummary>>* refresh_ = nullptr;
    QTableWidget* table_ = nullptr;
    QComboBox* battleFilter_ = nullptr;
    QComboBox* completionFilter_ = nullptr;
    QLineEdit* search_ = nullptr;
    QLabel* detail_ = nullptr;
    QPushButton* record_ = nullptr;
    QPushButton* openWorkflow_ = nullptr;
    QPushButton* openJob_ = nullptr;
    QPushButton* openArtifact_ = nullptr;
    std::vector<savorqt::db::VictoryResultSummary> rows_;
    savorqt::gui::KeyedSelection<std::int64_t> selectedVictory_;
    bool initialHydration_ = true;
    std::int64_t routeNodeId_ = 0;
};
