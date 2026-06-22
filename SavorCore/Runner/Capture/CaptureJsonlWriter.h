#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace savor::capture {

struct CaptureField {
    std::string name;
    std::string value;
    bool quote = false;
};

struct CheckpointCaptureRecord {
    std::uint64_t capture_sequence = 0;
    std::uint64_t checkpoint_hit_count = 0;
    std::uint32_t pc = 0;
    std::string stop_kind = "pc_breakpoint";
    std::string checkpoint_id;
    std::string checkpoint_name;
    std::string function;
    std::string checkpoint;
    std::uint64_t movie_input_count = 0;
    std::uint64_t vi_field_count = 0;
    std::uint64_t frame_count = 0;
    std::uint32_t tbr_high = 0;
    std::uint32_t tbr_low = 0;
    std::uint64_t tbr_u64 = 0;
    std::uint32_t rng_draw_index_before = 0;
    bool owns_rng_draw = false;
    std::vector<CaptureField> fields;
};

std::string JsonEscape(const std::string& value);
std::string HexU32(std::uint32_t value);
std::string HexU64(std::uint64_t value);
std::string SerializeJsonlRecord(const CheckpointCaptureRecord& record);

class CaptureJsonlWriter {
public:
    bool open(const std::filesystem::path& path, std::string* error_out = nullptr);
    bool write(const CheckpointCaptureRecord& record, std::string* error_out = nullptr);
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    std::ofstream out_;
};

} // namespace savor::capture
