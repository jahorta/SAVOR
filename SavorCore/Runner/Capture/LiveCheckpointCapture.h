#pragma once

#include "CaptureJsonlWriter.h"
#include "CaptureProfile.h"
#include "../../Core/DolphinWrapper.h"

#include <cstdint>
#include <filesystem>
#include <unordered_map>

namespace savor::capture {

class LiveCheckpointCapture {
public:
    bool start(
        const std::filesystem::path& profile_path,
        const std::filesystem::path& output_path,
        std::string* error_out = nullptr);
    void stop();

    bool active() const { return active_; }
    bool contains_pc(std::uint32_t pc) const;
    std::vector<std::uint32_t> pcs() const;
    const std::vector<MemoryWatchpointSpec>& memory_watchpoints() const;
    const std::filesystem::path& output_path() const { return output_path_; }

    bool capture_hit(DolphinWrapper& host, std::uint32_t pc, std::string* error_out = nullptr);
    bool capture_memory_watchpoint_hit(
        DolphinWrapper& host,
        const std::string& stop_kind,
        const DolphinWrapper::MemoryWatchpointHit& hit,
        std::string* error_out = nullptr);
    bool capture_memory_watchpoint_delta(
        DolphinWrapper& host,
        const std::string& stop_kind,
        const DolphinWrapper::MemoryWatchpointDelta& delta,
        const DolphinWrapper::DecodedMemoryAccess& decoded_current_access,
        std::string* error_out = nullptr);
    bool capture_seed_override(
        DolphinWrapper& host,
        std::uint32_t pc,
        std::uint32_t original_seed,
        std::uint32_t override_seed,
        std::uint32_t applied_seed,
        std::string* error_out = nullptr);

private:
    bool active_ = false;
    CaptureProfile profile_;
    CaptureJsonlWriter writer_;
    std::filesystem::path output_path_;
    std::uint64_t next_sequence_ = 0;
    std::uint32_t rng_draw_index_ = 0;
    std::unordered_map<std::string, std::uint64_t> hit_counts_;
};

} // namespace savor::capture
