#pragma once

#include "GUI/Workspace/WorkspaceWidgets.h"

#include <functional>
#include <cstdint>

class QComboBox;
class QGraphicsScene;
class QGraphicsView;
class QLabel;
class QLineEdit;
class QPushButton;
namespace savorqt::gui { template <typename Request, typename Result> class AsyncRefreshPipeline; }
namespace savorqt::db { struct TasRouteSnapshot; }

class TasRoutesTab final : public savorqt::gui::WorkspacePageShell {
public:
    struct Actions {
        std::function<void(std::int64_t)> openVictoryResults;
        std::function<void(std::int64_t)> openWorkflow;
    };

    explicit TasRoutesTab(Actions actions, QWidget* parent = nullptr);
    void setPageActive(bool active);
    void requestRefresh();

private:
    void applySnapshot(const savorqt::db::TasRouteSnapshot& snapshot);
    void selectNode(std::int64_t route_node_id);
    void openSelected();

    Actions actions_;
    savorqt::gui::AsyncRefreshPipeline<int, savorqt::db::TasRouteSnapshot>* refresh_ = nullptr;
    QGraphicsScene* scene_ = nullptr;
    QGraphicsView* view_ = nullptr;
    QComboBox* rootFilter_ = nullptr;
    QLineEdit* search_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* detail_ = nullptr;
    QLineEdit* labelEdit_ = nullptr;
    QPushButton* openVictories_ = nullptr;
    std::int64_t selectedNodeId_ = 0;
    bool selectedIsBattle_ = false;
    bool active_ = false;
    savorqt::db::TasRouteSnapshot* snapshot_ = nullptr;
};
