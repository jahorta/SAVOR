#pragma once

#include "../DBCore/DbResult.h"
#include "../DBCore/DbRetryPolicy.h"
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <vector>

namespace simcore::db {

struct VisualReplayEventRow {
    int64_t visual_event_id{};
    int64_t visual_replay_id{};
    std::string event_kind;
    std::optional<std::string> payload{};
    int64_t created_at{};
};

class VisualReplayEventsRepo {
public:
    static std::future<DbResult<int64_t>> AppendAsync(int64_t visual_replay_id, const std::string& event_kind, std::optional<std::string> payload = std::nullopt, RetryPolicy rp = {});
    static std::future<DbResult<std::vector<VisualReplayEventRow>>> ListByReplayAsync(int64_t visual_replay_id, RetryPolicy rp = {});

    static inline DbResult<int64_t> Append(int64_t visual_replay_id, const std::string& event_kind, std::optional<std::string> payload = std::nullopt) {
        return AppendAsync(visual_replay_id, event_kind, std::move(payload)).get();
    }
    static inline DbResult<std::vector<VisualReplayEventRow>> ListByReplay(int64_t visual_replay_id) {
        return ListByReplayAsync(visual_replay_id).get();
    }
};

} // namespace simcore::db
