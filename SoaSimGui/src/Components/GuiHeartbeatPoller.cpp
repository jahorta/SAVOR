#include "GuiHeartbeatPoller.h"
#include "DB/DBCore/DbService.h"
#include "DB/DBCore/DbResult.h"
#include "DB/DBCore/DbRetryPolicy.h"
#include "DB/DBCore/DbEnv.h"
#include <chrono>

using namespace std::chrono;
using simcore::db::DBService;
using simcore::db::DbEnv;
using simcore::db::OpType;
using simcore::db::Priority;
using simcore::db::RetryPolicy;
using simcore::db::DbResult;

void GuiHeartbeatPoller::start(GuiStatusModel* model) {
    if (running_.exchange(true)) return;
    model_ = model;
    th_ = std::thread(&GuiHeartbeatPoller::run, this);
}

void GuiHeartbeatPoller::stop() {
    if (!running_.exchange(false)) return;
    if (th_.joinable()) th_.join();
}

void GuiHeartbeatPoller::run() {
    GuiStatusSnapshot snap{};
    snap.env_label = "prod";
    while (running_) {
        auto fut = DBService::instance().submit_res<int>(
            OpType::Read, Priority::High, RetryPolicy{},
            [](DbEnv&) -> DbResult<int> { return DbResult<int>::Ok(1); });

        bool ok = (fut.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
        if (ok) {
            auto r = fut.get();
            snap.connected = r.ok;
            if (r.ok) {
                snap.last_heartbeat_tp = std::chrono::steady_clock::now();
                snap.last_error.clear();
            }
            else {
                snap.connected = false;
                snap.last_error = "DB error";
            }
        }
        else {
            snap.connected = false;
            snap.last_error = "Heartbeat timeout";
        }
        if (model_) model_->set(snap);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}
