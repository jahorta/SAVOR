#include "DebugControlChannel.h"

#include <algorithm>
#include <chrono>

namespace {
    static int64_t now_sec() {
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }
}

namespace simcore::debug {

    LocalDebugControlServer::LocalDebugControlServer(int64_t session_id, int64_t job_id, std::string token, std::string vm_endpoint, std::string dolphin_endpoint, uint32_t video_width, uint32_t video_height, std::string video_pixel_format, std::string video_color_space)
        : session_id_(session_id),
        token_(std::move(token)),
        vm_endpoint_(std::move(vm_endpoint)),
        dolphin_endpoint_(std::move(dolphin_endpoint)) {
        snapshot_.session_id = session_id;
        snapshot_.job_id = job_id;
        snapshot_.vm_state = "VM_PAUSED";
        snapshot_.emu_state = "EMU_PAUSED";
        snapshot_.ux_mode = "FRAME_STEP_DEFAULT";
        snapshot_.break_reason = "paused";
        snapshot_.script_name = "Script/main";
        snapshot_.script_pc = 0;
        snapshot_.frame_index = 0;
        snapshot_.current_input = "A=0 B=0 X=0 Y=0";
        snapshot_.sequence = 1;
        snapshot_.timestamp = now_sec();
        snapshot_.frame_ready = false;
        snapshot_.video_pixel_format = video_pixel_format.empty() ? "UNKNOWN" : std::move(video_pixel_format);
        snapshot_.video_color_space = video_color_space.empty() ? "UNKNOWN" : std::move(video_color_space);
        snapshot_.video_ring_name = MakeVideoRingMappingName(session_id, token_);
        snapshot_.video_width = video_width > 0 ? video_width : 640;
        snapshot_.video_height = video_height > 0 ? video_height : 480;

        VideoRingConfig cfg{};
        cfg.mapping_name = snapshot_.video_ring_name;
        cfg.slot_count = 4;
        cfg.max_frame_bytes = snapshot_.video_width * snapshot_.video_height * 4;
        (void)video_ring_.Open(cfg);

        frame_scratch_.resize(static_cast<size_t>(snapshot_.video_width) * static_cast<size_t>(snapshot_.video_height) * 4u);
        publish_frame_(snapshot_.video_width, snapshot_.video_height, 0);
    }

    LocalDebugControlServer::~LocalDebugControlServer() {
        Stop();
    }

    void LocalDebugControlServer::Stop() {
        stop_.store(true);
        interrupt_requested_.store(true);
        if (run_thread_.joinable()) run_thread_.join();
        run_active_.store(false);
        video_ring_.Close();
    }

    std::string LocalDebugControlServer::Token() const { return token_; }
    std::string LocalDebugControlServer::VmEndpoint() const { return vm_endpoint_; }
    std::string LocalDebugControlServer::DolphinEndpoint() const { return dolphin_endpoint_; }

    void LocalDebugControlServer::interrupt_and_wait_() {
        if (!run_active_.load()) return;
        interrupt_requested_.store(true);
        if (run_thread_.joinable()) run_thread_.join();
        run_active_.store(false);
        std::lock_guard<std::mutex> lk(mu_);
        snapshot_.emu_state = "EMU_PAUSED";
        snapshot_.ux_mode = "FRAME_STEP_DEFAULT";
        snapshot_.break_reason = "paused";
        snapshot_.sequence++;
        snapshot_.timestamp = now_sec();
    }

    simcore::db::DbResult<void> LocalDebugControlServer::Send(const std::string& endpoint, const std::string& token, const ControlCommand& cmd) {
        if (cmd.env.protocol_version != 1) return simcore::db::DbResult<void>::Err({ simcore::db::DbErrorKind::InvalidArgument, -1, "ProtocolVersionMismatch" });
        if (cmd.env.session_id != session_id_) return simcore::db::DbResult<void>::Err({ simcore::db::DbErrorKind::InvalidArgument, -1, "SessionMismatch" });
        if (token != token_) return simcore::db::DbResult<void>::Err({ simcore::db::DbErrorKind::InvalidArgument, -1, "InvalidSessionToken" });

        const bool is_vm_endpoint = endpoint == vm_endpoint_;
        const bool is_dolphin_endpoint = endpoint == dolphin_endpoint_;
        if (!is_vm_endpoint && !is_dolphin_endpoint) return simcore::db::DbResult<void>::Err({ simcore::db::DbErrorKind::NotFound, -1, "EndpointNotFound" });

        switch (cmd.env.command) {
        case CommandType::StepVmInstruction: {
            interrupt_and_wait_();
            std::lock_guard<std::mutex> lk(mu_);
            snapshot_.vm_state = "VM_STEPPING_INSTR";
            snapshot_.ux_mode = "VM_INSTR_DEBUG";
            snapshot_.script_pc += 4;
            snapshot_.break_reason = "vm_step";
            snapshot_.sequence++;
            snapshot_.vm_state = "VM_PAUSED";
            snapshot_.timestamp = now_sec();
            return simcore::db::DbResult<void>::Ok();
        }
        case CommandType::StepFrame:
        case CommandType::SetModeFrameStep: {
            interrupt_and_wait_();
            std::lock_guard<std::mutex> lk(mu_);
            snapshot_.emu_state = "EMU_FRAME_STEP";
            snapshot_.ux_mode = "FRAME_STEP_DEFAULT";
            snapshot_.frame_index += 1;
            snapshot_.current_input = "A=1 B=0 X=0 Y=0";
            snapshot_.frame_ready = true;
            snapshot_.break_reason = "frame_step";
            snapshot_.sequence++;
            snapshot_.emu_state = "EMU_PAUSED";
            snapshot_.timestamp = now_sec();
            publish_frame_(snapshot_.video_width, snapshot_.video_height, static_cast<uint8_t>(snapshot_.frame_index & 0xFF));
            return simcore::db::DbResult<void>::Ok();
        }
        case CommandType::RunToBreakpoint: {
            if (run_active_.load()) return simcore::db::DbResult<void>::Err({ simcore::db::DbErrorKind::Constraint, -1, "RunAlreadyActive" });
            interrupt_requested_.store(false);
            run_active_.store(true);
            {
                std::lock_guard<std::mutex> lk(mu_);
                snapshot_.emu_state = "EMU_RUN_TO_BP";
                snapshot_.ux_mode = "RUN_TO_BP_ACTIVE";
                snapshot_.break_reason = "running";
                snapshot_.sequence++;
                snapshot_.timestamp = now_sec();
            }
            if (run_thread_.joinable()) run_thread_.join();
            run_thread_ = std::thread([this]() { run_to_bp_loop_(); });
            return simcore::db::DbResult<void>::Ok();
        }
        case CommandType::Pause: {
            interrupt_and_wait_();
            std::lock_guard<std::mutex> lk(mu_);
            snapshot_.vm_state = "VM_PAUSED";
            snapshot_.emu_state = "EMU_PAUSED";
            snapshot_.ux_mode = "FRAME_STEP_DEFAULT";
            snapshot_.break_reason = "paused";
            snapshot_.sequence++;
            snapshot_.timestamp = now_sec();
            return simcore::db::DbResult<void>::Ok();
        }
        case CommandType::ToggleBreakpoint: {
            std::lock_guard<std::mutex> lk(mu_);
            if (cmd.enabled) breakpoints_.insert(cmd.step_id);
            else breakpoints_.erase(cmd.step_id);
            snapshot_.breakpoints.assign(breakpoints_.begin(), breakpoints_.end());
            std::sort(snapshot_.breakpoints.begin(), snapshot_.breakpoints.end());
            snapshot_.sequence++;
            snapshot_.timestamp = now_sec();
            return simcore::db::DbResult<void>::Ok();
        }
        default:
            return simcore::db::DbResult<void>::Err({ simcore::db::DbErrorKind::InvalidArgument, -1, "UnsupportedCommand" });
        }
    }

    void LocalDebugControlServer::run_to_bp_loop_() {
        bool hit_breakpoint = false;
        for (int i = 0; i < 30; ++i) {
            if (stop_.load() || interrupt_requested_.load()) break;
            {
                std::lock_guard<std::mutex> lk(mu_);
                snapshot_.frame_index += 1;
                snapshot_.script_pc += 4;
                snapshot_.current_input = (i % 2 == 0) ? "A=0 B=1 X=0 Y=0" : "A=1 B=0 X=0 Y=0";
                snapshot_.frame_ready = true;
                snapshot_.sequence++;
                snapshot_.timestamp = now_sec();
                publish_frame_(snapshot_.video_width, snapshot_.video_height, static_cast<uint8_t>((snapshot_.frame_index * 3) & 0xFF));
                if (!breakpoints_.empty() && (i % 5 == 4)) {
                    hit_breakpoint = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            snapshot_.emu_state = "EMU_PAUSED";
            snapshot_.ux_mode = "FRAME_STEP_DEFAULT";
            if (interrupt_requested_.load()) snapshot_.break_reason = "interrupted_to_frame_step";
            else snapshot_.break_reason = hit_breakpoint ? "breakpoint_hit" : "run_to_bp_timeout";
            snapshot_.sequence++;
            snapshot_.timestamp = now_sec();
        }

        run_active_.store(false);
    }

    simcore::db::DbResult<simcore::WorkerCoordinator::DebugRuntimeSnapshot> LocalDebugControlServer::Snapshot() const {
        std::lock_guard<std::mutex> lk(mu_);
        return simcore::db::DbResult<simcore::WorkerCoordinator::DebugRuntimeSnapshot>::Ok(snapshot_);
    }

    void LocalDebugControlServer::publish_frame_(uint32_t width, uint32_t height, uint8_t phase) {
        if (!video_ring_.IsOpen()) return;
        const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        if (frame_scratch_.size() != expected) frame_scratch_.resize(expected);

        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const size_t o = (static_cast<size_t>(y) * width + x) * 4u;
                frame_scratch_[o + 0] = static_cast<uint8_t>((x + phase) & 0xFF); // B
                frame_scratch_[o + 1] = static_cast<uint8_t>((y + (phase * 2)) & 0xFF); // G
                frame_scratch_[o + 2] = static_cast<uint8_t>((phase * 5) & 0xFF); // R
                frame_scratch_[o + 3] = 255;
            }
        }

        VideoFrameDesc d{};
        d.frame_id = next_frame_id_++;
        d.timestamp_sec = now_sec();
        d.width = width;
        d.height = height;
        d.stride = width * 4u;
        d.format = VideoPixelFormat::BGRA8;
        d.color_space = 1;
        d.flags = 0;
        d.data_bytes = static_cast<uint32_t>(frame_scratch_.size());
        (void)video_ring_.WriteFrame(d, frame_scratch_.data(), frame_scratch_.size());
    }

} // namespace simcore::debug
