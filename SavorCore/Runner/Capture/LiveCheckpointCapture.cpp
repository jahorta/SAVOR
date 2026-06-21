#include "LiveCheckpointCapture.h"

#include "../../Core/DolphinWrapper.h"

#include <algorithm>
#include <string>

namespace savor::capture {
namespace {

bool read_memory_sample(DolphinWrapper& host, const MemorySampleSpec& spec, std::uint64_t& out)
{
    switch (spec.width) {
    case SampleWidth::U8: {
        std::uint8_t v = 0;
        if (!host.readU8(spec.address, v)) return false;
        out = v;
        return true;
    }
    case SampleWidth::U16: {
        std::uint16_t v = 0;
        if (!host.readU16(spec.address, v)) return false;
        out = v;
        return true;
    }
    case SampleWidth::U32: {
        std::uint32_t v = 0;
        if (!host.readU32(spec.address, v)) return false;
        out = v;
        return true;
    }
    case SampleWidth::U64: {
        std::uint64_t v = 0;
        if (!host.readU64(spec.address, v)) return false;
        out = v;
        return true;
    }
    default:
        return false;
    }
}

std::string sample_value_for_json(std::uint64_t value, SampleWidth width)
{
    if (width == SampleWidth::U64) {
        return "\"" + HexU64(value) + "\"";
    }
    return "\"" + HexU32(static_cast<std::uint32_t>(value)) + "\"";
}

} // namespace

bool LiveCheckpointCapture::start(
    const std::filesystem::path& profile_path,
    const std::filesystem::path& output_path,
    std::string* error_out)
{
    stop();

    auto parsed = LoadCaptureProfileFile(profile_path.string());
    if (!parsed.profile.has_value()) {
        if (error_out) *error_out = FormatCaptureProfileError(parsed);
        return false;
    }
    CaptureJsonlWriter writer;
    if (!writer.open(output_path, error_out)) {
        return false;
    }

    profile_ = std::move(*parsed.profile);
    writer_ = std::move(writer);
    output_path_ = output_path;
    next_sequence_ = 0;
    rng_draw_index_ = 0;
    hit_counts_.clear();
    active_ = true;
    return true;
}

void LiveCheckpointCapture::stop()
{
    active_ = false;
    profile_ = CaptureProfile{};
    writer_ = CaptureJsonlWriter{};
    output_path_.clear();
    next_sequence_ = 0;
    rng_draw_index_ = 0;
    hit_counts_.clear();
}

bool LiveCheckpointCapture::contains_pc(std::uint32_t pc) const
{
    return active_ && profile_.find_checkpoint(pc) != nullptr;
}

std::vector<std::uint32_t> LiveCheckpointCapture::pcs() const
{
    return active_ ? profile_.pcs() : std::vector<std::uint32_t>{};
}

bool LiveCheckpointCapture::capture_hit(DolphinWrapper& host, std::uint32_t pc, std::string* error_out)
{
    if (!active_) return true;
    const auto* checkpoint = profile_.find_checkpoint(pc);
    if (checkpoint == nullptr) return true;

    CheckpointCaptureRecord record{};
    record.capture_sequence = next_sequence_++;
    record.pc = pc;
    record.checkpoint_id = checkpoint->id;
    record.checkpoint_name = checkpoint->name;
    record.function = checkpoint->function;
    record.checkpoint = checkpoint->checkpoint;
    record.movie_input_count = host.getCurrentMovieInputCount();
    record.vi_field_count = host.getViFieldCountApprox();
    record.frame_count = host.getFrameCountApprox(false);
    record.tbr_u64 = host.getTBR();
    record.tbr_high = static_cast<std::uint32_t>(record.tbr_u64 >> 32);
    record.tbr_low = static_cast<std::uint32_t>(record.tbr_u64);
    record.rng_draw_index_before = rng_draw_index_;
    record.owns_rng_draw = checkpoint->owns_rng_draw;

    auto& hit_count = hit_counts_[checkpoint->id];
    record.checkpoint_hit_count = hit_count++;

    for (const auto& sample : checkpoint->memory_samples) {
        std::uint64_t value = 0;
        if (!read_memory_sample(host, sample, value)) {
            record.fields.push_back(CaptureField{ sample.name + "_read_ok", "false", false });
            continue;
        }
        record.fields.push_back(CaptureField{ sample.name, sample_value_for_json(value, sample.width), false });
    }

    for (const auto& sample : checkpoint->gpr_samples) {
        const auto value = host.getRegister(sample.reg);
        record.fields.push_back(CaptureField{ sample.name, "\"" + HexU32(value) + "\"", false });
    }

    for (const auto& sample : checkpoint->register_memory_samples) {
        const auto base = host.getRegister(sample.base_reg);
        const auto address = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(base) + static_cast<std::int64_t>(sample.offset));
        record.fields.push_back(CaptureField{ sample.name + "_address", "\"" + HexU32(address) + "\"", false });

        std::uint64_t value = 0;
        if (!read_memory_sample(host, MemorySampleSpec{ sample.name, address, sample.width }, value)) {
            record.fields.push_back(CaptureField{ sample.name + "_read_ok", "false", false });
            continue;
        }
        record.fields.push_back(CaptureField{ sample.name, sample_value_for_json(value, sample.width), false });
    }

    if (!writer_.write(record, error_out)) {
        return false;
    }
    if (checkpoint->owns_rng_draw) {
        ++rng_draw_index_;
    }
    return true;
}

} // namespace savor::capture
