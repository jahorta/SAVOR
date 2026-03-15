#pragma once

#include "../../DB/DBCore/DbResult.h"
#include "../Parallel/DB/DBWorkerCoordinator.h"
#include "VideoFrameRing.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace simcore::debug {

    enum class EndpointType : uint8_t {
        VM = 1,
        Dolphin = 2
    };

    enum class CommandType : uint8_t {
        StepVmInstruction = 1,
        StepFrame = 2,
        RunToBreakpoint = 3,
        Pause = 4,
        ToggleBreakpoint = 5,
        SetModeFrameStep = 6
    };

    struct ControlEnvelope {
        uint16_t protocol_version{ 1 };
        int64_t session_id{ 0 };
        EndpointType endpoint{ EndpointType::VM };
        CommandType command{ CommandType::Pause };
        int64_t correlation_id{ 0 };
    };

    struct ControlCommand {
        ControlEnvelope env{};
        int64_t step_id{ 0 };
        bool enabled{ false };
    };

    class LocalDebugControlServer {
    public:
        LocalDebugControlServer(int64_t session_id, int64_t job_id, std::string token, std::string vm_endpoint, std::string dolphin_endpoint);
        ~LocalDebugControlServer();

        simcore::db::DbResult<void> Send(const std::string& endpoint, const std::string& token, const ControlCommand& cmd);
        simcore::db::DbResult<simcore::WorkerCoordinator::DebugRuntimeSnapshot> Snapshot() const;

        std::string Token() const;
        std::string VmEndpoint() const;
        std::string DolphinEndpoint() const;

        void Stop();

    private:
        void run_to_bp_loop_();
        void interrupt_and_wait_();

        int64_t session_id_{ 0 };
        std::string token_;
        std::string vm_endpoint_;
        std::string dolphin_endpoint_;

        mutable std::mutex mu_;
        std::condition_variable cv_;
        simcore::WorkerCoordinator::DebugRuntimeSnapshot snapshot_{};
        std::set<int64_t> breakpoints_;
        VideoFrameRingProducer video_ring_;
        uint64_t next_frame_id_{ 1 };
        std::vector<uint8_t> frame_scratch_;

        void publish_frame_(uint32_t width, uint32_t height, uint8_t phase);

        std::atomic<bool> stop_{ false };
        std::atomic<bool> run_active_{ false };
        std::atomic<bool> interrupt_requested_{ false };
        std::thread run_thread_;
    };

} // namespace simcore::debug
