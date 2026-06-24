#include "LiveCheckpointCapture.h"

#include "../../Core/DolphinWrapper.h"
#include "../../Core/Memory/Soa/SoaAddrProgram.h"

#include <algorithm>
#include <cstddef>
#include <sstream>
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

std::string watchpoint_access_for_json(std::uint32_t access)
{
    switch (access) {
    case 1: return "read";
    case 2: return "write";
    case 3: return "access";
    default: return "unknown";
    }
}

std::string gpr_name(std::uint8_t reg)
{
    return "r" + std::to_string(static_cast<unsigned>(reg));
}

std::string watchpoint_access_for_json(WatchpointAccess access)
{
    return watchpoint_access_for_json(static_cast<std::uint32_t>(access));
}

std::string watchpoint_label_for_id(
    const std::vector<LiveCheckpointCapture::ActiveMemoryWatchpointSpec>& watchpoints,
    std::uint32_t id)
{
    for (const auto& watchpoint : watchpoints) {
        if (watchpoint.id == id) {
            return watchpoint.label;
        }
    }
    return std::to_string(id);
}

const LiveCheckpointCapture::ActiveMemoryWatchpointSpec* watchpoint_for_id(
    const std::vector<LiveCheckpointCapture::ActiveMemoryWatchpointSpec>& watchpoints,
    std::uint32_t id)
{
    for (const auto& watchpoint : watchpoints) {
        if (watchpoint.id == id) return &watchpoint;
    }
    return nullptr;
}

void append_decoded_memory_access_fields(
    CheckpointCaptureRecord& record,
    const DolphinWrapper::DecodedMemoryAccess& decoded)
{
    record.fields.push_back(CaptureField{ "decoded_pc", "\"" + HexU32(decoded.pc) + "\"", false });
    record.fields.push_back(CaptureField{ "decoded_opcode", "\"" + HexU32(decoded.opcode) + "\"", false });
    record.fields.push_back(CaptureField{ "decoded_mnemonic", decoded.mnemonic, true });
    record.fields.push_back(CaptureField{ "decoded_supported", decoded.supported ? "true" : "false", false });
    record.fields.push_back(CaptureField{ "decoded_is_memory_access", decoded.is_memory_access ? "true" : "false", false });
    if (!decoded.unsupported_reason.empty()) {
        record.fields.push_back(CaptureField{ "decoded_unsupported_reason", decoded.unsupported_reason, true });
    }
    if (decoded.is_memory_access) {
        record.fields.push_back(CaptureField{
            "decoded_access",
            watchpoint_access_for_json(static_cast<std::uint32_t>(decoded.access)),
            true });
        record.fields.push_back(CaptureField{ "decoded_effective_addr", "\"" + HexU32(decoded.effective_address) + "\"", false });
        record.fields.push_back(CaptureField{ "decoded_access_size", std::to_string(decoded.access_size), false });
    }
    if (decoded.has_base_reg) {
        record.fields.push_back(CaptureField{ "decoded_base_reg", gpr_name(decoded.base_reg), true });
    }
    if (decoded.has_index_reg) {
        record.fields.push_back(CaptureField{ "decoded_index_reg", gpr_name(decoded.index_reg), true });
    }
    if (decoded.has_value_reg) {
        record.fields.push_back(CaptureField{ "decoded_value_reg", gpr_name(decoded.value_reg), true });
    }
    if (decoded.value_available) {
        record.fields.push_back(CaptureField{ "decoded_value", "\"" + HexU64(decoded.value) + "\"", false });
    }
    if (!decoded.value_source.empty()) {
        record.fields.push_back(CaptureField{ "decoded_value_source", decoded.value_source, true });
    }
    if (decoded.memory_value_available) {
        record.fields.push_back(CaptureField{ "decoded_memory_value", "\"" + HexU64(decoded.memory_value) + "\"", false });
    }
    for (const auto& reg : decoded.gpr_values) {
        const std::string prefix = "decoded_gpr_" + gpr_name(reg.reg);
        record.fields.push_back(CaptureField{ prefix + "_role", reg.role, true });
        record.fields.push_back(CaptureField{ prefix + "_value", "\"" + HexU32(reg.value) + "\"", false });
    }
}

void append_address_program_trace_fields(
    CheckpointCaptureRecord& record,
    const std::string& name,
    const addrprog::EvalResult& eval)
{
    for (std::size_t i = 0; i < eval.trace.size(); ++i) {
        const auto& step = eval.trace[i];
        const auto prefix = name + "_step" + std::to_string(i);
        record.fields.push_back(CaptureField{ prefix + "_op", step.op_name, true });
        record.fields.push_back(CaptureField{ prefix + "_address_before", "\"" + HexU32(step.address_before) + "\"", false });
        record.fields.push_back(CaptureField{ prefix + "_address_after", "\"" + HexU32(step.address_after) + "\"", false });
        if (step.has_value) {
            record.fields.push_back(CaptureField{ prefix + "_value", "\"" + HexU64(step.value) + "\"", false });
        }
    }
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
    active_memory_watchpoints_.clear();
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
    active_memory_watchpoints_.clear();
}

bool LiveCheckpointCapture::contains_pc(std::uint32_t pc) const
{
    return active_ && !profile_.find_checkpoints(pc).empty();
}

std::vector<std::uint32_t> LiveCheckpointCapture::pcs() const
{
    return active_ ? profile_.pcs() : std::vector<std::uint32_t>{};
}

const std::vector<MemoryWatchpointSpec>& LiveCheckpointCapture::memory_watchpoints() const
{
    return profile_.memory_watchpoints;
}

std::vector<LiveCheckpointCapture::ActiveMemoryWatchpointSpec>
LiveCheckpointCapture::static_memory_watchpoints(WatchpointScope scope) const
{
    std::vector<ActiveMemoryWatchpointSpec> out;
    std::uint32_t id = 1;
    for (const auto& watchpoint : profile_.memory_watchpoints) {
        if (watchpoint.scope != scope) {
            ++id;
            continue;
        }
        out.push_back(ActiveMemoryWatchpointSpec{
            .id = id,
            .label = watchpoint.id,
            .address = watchpoint.address,
            .size = static_cast<std::uint32_t>(watchpoint.size),
            .access = watchpoint.access,
            .scope = watchpoint.scope,
            .source_pc = 0u,
            .source_checkpoint_id = "profile_static",
            .one_shot = false,
        });
        ++id;
    }
    return out;
}

void LiveCheckpointCapture::set_active_memory_watchpoints(
    std::vector<ActiveMemoryWatchpointSpec> watchpoints)
{
    active_memory_watchpoints_ = std::move(watchpoints);
}

std::vector<LiveCheckpointCapture::ActiveMemoryWatchpointSpec>
LiveCheckpointCapture::derive_dynamic_memory_watchpoints(
    DolphinWrapper& host,
    std::uint32_t pc,
    WatchpointScope scope) const
{
    std::vector<ActiveMemoryWatchpointSpec> out;
    if (!active_) return out;

    const auto address_already_active = [&](std::uint32_t address) {
        return std::any_of(
            active_memory_watchpoints_.begin(),
            active_memory_watchpoints_.end(),
            [&](const ActiveMemoryWatchpointSpec& active) {
                return active.address == address;
            });
    };
    const auto find_pending_by_address = [&](std::uint32_t address)
        -> ActiveMemoryWatchpointSpec* {
        for (auto& pending : out) {
            if (pending.address == address) return &pending;
        }
        return nullptr;
    };
    auto next_active_id = [&]() {
        std::uint32_t next_id = 1;
        for (const auto& active : active_memory_watchpoints_) {
            next_id = std::max(next_id, active.id + 1u);
        }
        for (const auto& pending : out) {
            next_id = std::max(next_id, pending.id + 1u);
        }
        return next_id;
    };

    for (const auto& spec : profile_.dynamic_memory_watchpoints) {
        if (spec.pc != pc || spec.scope != scope) continue;

        std::uint32_t address = spec.address;
        if (spec.use_address_program) {
            const auto eval = addrprog::evaluate(
                spec.address_program.data(),
                spec.address_program.size(),
                0,
                host,
                nullptr);
            if (!eval.ok) {
                continue;
            }
            address = eval.va;
        } else if (!spec.use_absolute_address) {
            const auto base = host.getRegister(spec.base_reg);
            const auto signed_address =
                static_cast<std::int64_t>(base) + static_cast<std::int64_t>(spec.offset);
            address = static_cast<std::uint32_t>(signed_address);
        }
        if (address == 0 || address_already_active(address)) {
            continue;
        }

        if (auto* existing = find_pending_by_address(address)) {
            existing->label += "|" + spec.id;
            existing->source_checkpoint_id += "|" + spec.id;
            continue;
        }

        ActiveMemoryWatchpointSpec active{};
        active.id = next_active_id();
        active.label = spec.id;
        active.address = address;
        active.size = static_cast<std::uint32_t>(spec.size);
        active.access = spec.access;
        active.scope = spec.scope;
        active.source_pc = spec.pc;
        active.source_checkpoint_id = spec.id;
        active.one_shot = spec.one_shot;
        out.push_back(std::move(active));
    }
    return out;
}

void LiveCheckpointCapture::append_active_memory_watchpoints(
    std::vector<ActiveMemoryWatchpointSpec> watchpoints)
{
    for (auto& watchpoint : watchpoints) {
        const auto duplicate = std::any_of(
            active_memory_watchpoints_.begin(),
            active_memory_watchpoints_.end(),
            [&](const ActiveMemoryWatchpointSpec& active) {
                return active.id == watchpoint.id || active.address == watchpoint.address;
            });
        if (!duplicate) {
            active_memory_watchpoints_.push_back(std::move(watchpoint));
        }
    }
}

bool LiveCheckpointCapture::active_memory_watchpoint_is_one_shot(std::uint32_t id) const
{
    if (const auto* active = watchpoint_for_id(active_memory_watchpoints_, id)) {
        return active->one_shot;
    }
    return false;
}

void LiveCheckpointCapture::remove_active_memory_watchpoint(std::uint32_t id)
{
    active_memory_watchpoints_.erase(
        std::remove_if(
            active_memory_watchpoints_.begin(),
            active_memory_watchpoints_.end(),
            [&](const ActiveMemoryWatchpointSpec& active) {
                return active.id == id;
            }),
        active_memory_watchpoints_.end());
}

bool LiveCheckpointCapture::capture_hit(DolphinWrapper& host, std::uint32_t pc, std::string* error_out)
{
    if (!active_) return true;
    const auto checkpoints = profile_.find_checkpoints(pc);
    if (checkpoints.empty()) return true;

    for (const auto* checkpoint : checkpoints) {
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

        for (const auto& sample : checkpoint->address_program_samples) {
            std::uint64_t value = 0;
            addrprog::EvalResult eval{};
            const auto read_ok = addrprog::read_value(
                sample.program.data(),
                sample.program.size(),
                0,
                host,
                nullptr,
                static_cast<std::uint8_t>(sample.width),
                value,
                &eval,
                [&](const std::uint8_t reg, std::uint32_t& out) {
                    out = host.getRegister(reg);
                    return true;
                },
                checkpoint->address_program_trace);
            record.fields.push_back(CaptureField{ sample.name + "_eval_ok", eval.ok ? "true" : "false", false });
            if (eval.ok) {
                record.fields.push_back(CaptureField{ sample.name + "_address", "\"" + HexU32(eval.va) + "\"", false });
            } else {
                record.fields.push_back(CaptureField{ sample.name + "_eval_error", eval.error, true });
            }
            record.fields.push_back(CaptureField{ sample.name + "_read_ok", read_ok ? "true" : "false", false });
            if (read_ok) {
                record.fields.push_back(CaptureField{ sample.name, sample_value_for_json(value, sample.width), false });
            }
            if (checkpoint->address_program_trace) {
                append_address_program_trace_fields(record, sample.name, eval);
            }
        }

        if (!writer_.write(record, error_out)) {
            return false;
        }
        if (checkpoint->owns_rng_draw) {
            ++rng_draw_index_;
        }
    }
    return true;
}

bool LiveCheckpointCapture::capture_memory_watchpoint_hit(
    DolphinWrapper& host,
    const std::string& stop_kind,
    const DolphinWrapper::MemoryWatchpointHit& hit,
    std::string* error_out)
{
    if (!active_) return true;

    const std::string label = watchpoint_label_for_id(active_memory_watchpoints_, hit.id);
    const std::string checkpoint_id = "memwatch." + label;

    CheckpointCaptureRecord record{};
    record.capture_sequence = next_sequence_++;
    record.pc = hit.hit_pc;
    record.stop_kind = stop_kind;
    record.checkpoint_id = checkpoint_id;
    record.checkpoint_name = label;
    record.function = "memory_watchpoint";
    record.checkpoint = watchpoint_access_for_json(static_cast<std::uint32_t>(hit.access));
    record.movie_input_count = host.getCurrentMovieInputCount();
    record.vi_field_count = host.getViFieldCountApprox();
    record.frame_count = host.getFrameCountApprox(false);
    record.tbr_u64 = host.getTBR();
    record.tbr_high = static_cast<std::uint32_t>(record.tbr_u64 >> 32);
    record.tbr_low = static_cast<std::uint32_t>(record.tbr_u64);
    record.rng_draw_index_before = rng_draw_index_;
    record.owns_rng_draw = false;

    auto& hit_count = hit_counts_[checkpoint_id];
    record.checkpoint_hit_count = hit_count++;

    record.fields.push_back(CaptureField{ "memwatch_id", std::to_string(hit.id), false });
    if (const auto* active = watchpoint_for_id(active_memory_watchpoints_, hit.id)) {
        record.fields.push_back(CaptureField{ "memwatch_label", active->label, true });
        record.fields.push_back(CaptureField{ "memwatch_source_pc", "\"" + HexU32(active->source_pc) + "\"", false });
        record.fields.push_back(CaptureField{ "memwatch_source_checkpoint_id", active->source_checkpoint_id, true });
    }
    record.fields.push_back(CaptureField{ "memwatch_addr", "\"" + HexU32(hit.address) + "\"", false });
    record.fields.push_back(CaptureField{ "memwatch_size", std::to_string(hit.size), false });
    record.fields.push_back(CaptureField{ "memwatch_access", watchpoint_access_for_json(static_cast<std::uint32_t>(hit.access)), true });
    record.fields.push_back(CaptureField{ "memwatch_hits_before", std::to_string(hit.num_hits_before), false });
    record.fields.push_back(CaptureField{ "memwatch_hits_after", std::to_string(hit.num_hits_after), false });
    record.fields.push_back(CaptureField{ "memwatch_confirmed_current_instruction", "true", false });
    record.fields.push_back(CaptureField{ "memwatch_unattributed_extra_hits", std::to_string(hit.unattributed_extra_hits), false });
    append_decoded_memory_access_fields(record, hit.decoded_access);

    return writer_.write(record, error_out);
}

bool LiveCheckpointCapture::capture_memory_watchpoint_delta(
    DolphinWrapper& host,
    const std::string& stop_kind,
    const DolphinWrapper::MemoryWatchpointDelta& delta,
    const DolphinWrapper::DecodedMemoryAccess& decoded_current_access,
    std::string* error_out)
{
    if (!active_) return true;

    const std::string label = watchpoint_label_for_id(active_memory_watchpoints_, delta.id);
    const std::string checkpoint_id = "memwatch_delta." + label;

    CheckpointCaptureRecord record{};
    record.capture_sequence = next_sequence_++;
    record.pc = delta.hit_pc;
    record.stop_kind = stop_kind;
    record.checkpoint_id = checkpoint_id;
    record.checkpoint_name = label;
    record.function = "memory_watchpoint_delta";
    record.checkpoint = "unattributed_delta";
    record.movie_input_count = host.getCurrentMovieInputCount();
    record.vi_field_count = host.getViFieldCountApprox();
    record.frame_count = host.getFrameCountApprox(false);
    record.tbr_u64 = host.getTBR();
    record.tbr_high = static_cast<std::uint32_t>(record.tbr_u64 >> 32);
    record.tbr_low = static_cast<std::uint32_t>(record.tbr_u64);
    record.rng_draw_index_before = rng_draw_index_;
    record.owns_rng_draw = false;

    auto& hit_count = hit_counts_[checkpoint_id];
    record.checkpoint_hit_count = hit_count++;

    record.fields.push_back(CaptureField{ "memwatch_id", std::to_string(delta.id), false });
    if (const auto* active = watchpoint_for_id(active_memory_watchpoints_, delta.id)) {
        record.fields.push_back(CaptureField{ "memwatch_label", active->label, true });
        record.fields.push_back(CaptureField{ "memwatch_source_pc", "\"" + HexU32(active->source_pc) + "\"", false });
        record.fields.push_back(CaptureField{ "memwatch_source_checkpoint_id", active->source_checkpoint_id, true });
    }
    record.fields.push_back(CaptureField{ "memwatch_addr", "\"" + HexU32(delta.address) + "\"", false });
    record.fields.push_back(CaptureField{ "memwatch_size", std::to_string(delta.size), false });
    record.fields.push_back(CaptureField{ "memwatch_access", watchpoint_access_for_json(static_cast<std::uint32_t>(delta.access)), true });
    record.fields.push_back(CaptureField{ "memwatch_hits_before", std::to_string(delta.num_hits_before), false });
    record.fields.push_back(CaptureField{ "memwatch_hits_after", std::to_string(delta.num_hits_after), false });
    record.fields.push_back(CaptureField{ "memwatch_confirmed_current_instruction", "false", false });
    append_decoded_memory_access_fields(record, decoded_current_access);

    return writer_.write(record, error_out);
}

bool LiveCheckpointCapture::capture_seed_override(
    DolphinWrapper& host,
    std::uint32_t pc,
    std::uint32_t original_seed,
    std::uint32_t override_seed,
    std::uint32_t applied_seed,
    std::string* error_out)
{
    if (!active_) return true;

    constexpr const char* kCheckpointId = "prebattle.seed_override";

    CheckpointCaptureRecord record{};
    record.capture_sequence = next_sequence_++;
    record.pc = pc;
    record.checkpoint_id = kCheckpointId;
    record.checkpoint_name = kCheckpointId;
    record.function = "prebattle";
    record.checkpoint = "seed_override";
    record.movie_input_count = host.getCurrentMovieInputCount();
    record.vi_field_count = host.getViFieldCountApprox();
    record.frame_count = host.getFrameCountApprox(false);
    record.tbr_u64 = host.getTBR();
    record.tbr_high = static_cast<std::uint32_t>(record.tbr_u64 >> 32);
    record.tbr_low = static_cast<std::uint32_t>(record.tbr_u64);
    record.rng_draw_index_before = rng_draw_index_;
    record.owns_rng_draw = false;

    auto& hit_count = hit_counts_[kCheckpointId];
    record.checkpoint_hit_count = hit_count++;

    record.fields.push_back(CaptureField{ "original_seed", "\"" + HexU32(original_seed) + "\"", false });
    record.fields.push_back(CaptureField{ "override_seed", "\"" + HexU32(override_seed) + "\"", false });
    record.fields.push_back(CaptureField{ "applied_seed", "\"" + HexU32(applied_seed) + "\"", false });
    record.fields.push_back(CaptureField{ "readback_matches", applied_seed == override_seed ? "true" : "false", false });

    return writer_.write(record, error_out);
}

} // namespace savor::capture
