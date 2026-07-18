#pragma once
#include "../framework.h"

#include <optional>
#include <string>
#include <functional>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <filesystem>
#include <vector>

#include "Input/InputPlan.h"
#include "Config/SimConfig.h"
#include "Core/InputCommon/GCPadStatus.h"
#include "Input/GCPadOverride.h"
#include "Core/Common/Buffer.h"
#include "Common/WindowSystemInfo.h"

namespace Core { class System; }
namespace addr { enum class AddrKey : uint16_t; struct DolphinAddr; }

namespace savor {

    struct DiscInfo {
        std::string game_id;  // 6 chars like "GSOE8P"
        std::string region;   // "NTSC-U", "NTSC-J", "PAL", etc.
    };

    class DolphinWrapper {
    public:
        DolphinWrapper();
        ~DolphinWrapper();

        DolphinWrapper(const DolphinWrapper&) = delete;
        DolphinWrapper& operator=(const DolphinWrapper&) = delete;

        // Helper: shutdown Core cleanly (paired with Init/boot).
        void shutdownCore();
        void shutdownAll();

        // State / lifecycle
        bool isRunning() const noexcept;
        void stop();
        Core::System* system() const noexcept { return m_system; }

        bool loadGame(const std::string& iso_path);
        bool loadSavestate(const std::string& state_path);
        bool saveSavestateBlocking(const std::string& state_path);
        bool saveScreenshotBlocking(const std::string& image_path, uint32_t timeout_ms = 3000);
        bool saveStateToBuffer(Common::UniqueBuffer<u8>& buffer);
        bool loadStateFromBuffer(Common::UniqueBuffer<u8>& buffer);

        // Disc metadata captured during last loadGame()
        std::optional<DiscInfo> getDiscInfo() const { return m_disc_info; }

        // Run a functor on the CPU thread (blocks). Returns false if not running.
        bool runOnCpuThread(const std::function<void()>& fn, const bool waitForCompletion = true) const;

        bool getMem1(std::string& out) const; // fills out with 24 MiB MEM1 snapshot
        bool getMem1RangeRaw(std::string& out, uint32_t va, uint32_t size) const;
        uint32_t getRegister(uint8_t reg);
        uint32_t getLinkRegister();
        struct PowerPcStackFrame {
            uint32_t depth = 0;
            uint32_t sp = 0;
            uint32_t next_sp = 0;
            uint32_t return_pc = 0;
            uint32_t callsite_pc = 0;
        };
        std::vector<PowerPcStackFrame> getPowerPcCallStack(uint32_t max_depth = 8);
        uint32_t getPC();
        uint64_t getTBR();
        uint32_t getConfigRTC(bool offset = true);
        uint32_t getEmulatedTime();
        std::string getCurrentSctFileTag() const;
        std::string getCurrentSctSection() const;


        // Input functions
        void setInputPlan(const InputPlan& p) { m_plan = p; m_cursor = 0; }
        void applyNextInputFrame();
        void setInput(const GCInputFrame& f);
        size_t remainingInputs() const { return (m_cursor < m_plan.size()) ? (m_plan.size() - m_cursor) : 0; }

        struct InputTapePlaybackOptions {
            uint32_t max_unacked_replays = 2;
            uint32_t frame_step_timeout_ms = 5000;
            bool safe_mode = false;
            const char* label = "input_tape";
        };

        struct InputTapePlaybackResult {
            bool ok = false;
            uint32_t failed_index = UINT32_MAX;
            uint32_t unacked_count = 0;
            InputPlan attempted_frames;
            std::vector<uint32_t> vi_durations;
        };

        InputTapePlaybackResult playInputTapeBlocking(
            const InputPlan& plan,
            const InputTapePlaybackOptions& options = {});

        bool stepOneOpcodeBlocking(int timeout_ms = 1000);
        bool stepOneFrameBlocking(int timeout_ms = 1000);

        // Returns an approximate VI field count since the last reset.
        uint64_t getViFieldCountApproxFromBaseline() const;
        uint64_t getFrameCountApprox(bool interlaced = false) const;
        void resetViCounterBaseline();
        uint64_t getViFieldCountApprox() const;

        // Isolated user/base management
        bool SetUserDirectory(const std::filesystem::path& user_dir);
        bool SetDolphinQtBaseDir(const std::filesystem::path& dolphin_base_dir,
            std::string* error_out = nullptr);
        const std::filesystem::path& GetUserDirectory() const { return m_user_dir; }
        const std::filesystem::path& GetDolphinQtBaseDir() const { return m_qt_base_dir; }
        void SetVisualMode(bool visual) { m_visual_mode = visual; }
        void SetRenderWidgetHandle(void* handle) { m_external_render_widget_handle = handle; }
        bool IsVisualMode() const { return m_visual_mode; }
        bool SyncFromDolphinQtBase(bool force = false, std::string* error_out = nullptr);
        bool EnsureReadyForSavestate(std::string* error_out = nullptr) {
            return SyncFromDolphinQtBase(/*force=*/false, error_out);
        }

        void ConfigurePortsStandardPadP1();
        bool QueryPadStatus(int port, GCPadStatus* out) const;

        bool ApplyConfig(const savor::SimConfig& cfg, std::string* error_out = nullptr);
        savor::SimConfig ExportConfig() const {
            return savor::SimConfig{ m_user_dir, m_qt_base_dir };
        }

        // public:
        bool readU8(uint32_t addr, uint8_t& out) const;
        bool readU16(uint32_t addr, uint16_t& out) const;
        bool readU32(uint32_t addr, uint32_t& out) const;
        bool readU64(uint32_t addr, uint64_t& out) const;
        bool readF32(uint32_t addr, float& out) const;
        bool readF64(uint32_t addr, double& out) const;
        bool writeU32(uint32_t addr, uint32_t value);

        // Resolve an address key to a VA using the paused core's memory (no MEM1 copy).
        bool resolveKey(addr::AddrKey k, uint32_t& out_va) const;
        bool resolveKeyWithBase(addr::AddrKey k, uint32_t base_override, uint32_t& out_va) const;

        // Typed reads by key (paused-only; soft-fail on missing/unresolved key).
        bool readByKey(addr::AddrKey k, uint8_t& out, bool require_paused = true) const;
        bool readByKey(addr::AddrKey k, uint16_t& out, bool require_paused = true) const;
        bool readByKey(addr::AddrKey k, uint32_t& out, bool require_paused = true) const;
        bool readByKey(addr::AddrKey k, uint64_t& out, bool require_paused = true) const;

        // Width-aware read into 64-bit bucket; returns the actual width via out_width (1,2,4,8).
        bool readByKeyAny(addr::AddrKey k, uint8_t width, uint64_t& out, uint8_t& out_width) const;

        enum class MemoryWatchpointAccess : uint32_t {
            Read = 1,
            Write = 2,
            Access = 3,
        };

        enum class DebugStopKind : uint32_t {
            None = 0,
            PcBreakpoint = 1,
            Memcheck = 2,
        };

        struct MemoryWatchpointSpec {
            uint32_t id = 0;
            uint32_t address = 0;
            uint32_t size = 0;
            MemoryWatchpointAccess access = MemoryWatchpointAccess::Write;
        };

        struct MemoryWatchpointHit {
            uint32_t id = 0;
            uint32_t address = 0;
            uint32_t size = 0;
            MemoryWatchpointAccess access = MemoryWatchpointAccess::Write;
            uint32_t hit_pc = 0;
            uint32_t num_hits_before = 0;
            uint32_t num_hits_after = 0;
            bool confirmed_current_instruction = false;
        };

        struct RunUntilHitResult {
            bool hit = false;
            uint32_t pc = 0;
            const char* reason = nullptr;
            DebugStopKind stop_kind = DebugStopKind::None;
            std::optional<MemoryWatchpointHit> memory_watchpoint;
        };
        bool armPcBreakpoints(const std::vector<uint32_t>& pcs);
        bool disarmPcBreakpoints(const std::vector<uint32_t>& pcs);
        void clearAllPcBreakpoints();
        bool setEnableBreakpoint(uint32_t pc, bool enabled);
        bool setEnableAllBreakpoints(bool enabled);
        bool setEnabledPcBreakpointsOnly(const std::vector<uint32_t>& enabled_pcs);
        bool armMemoryWatchpoints(const std::vector<MemoryWatchpointSpec>& specs);
        void clearMemoryWatchpoints();

        using ProgressSink = std::function<void(const char* text, const bool record)>;

        ProgressSink getProgressSink() const;
        void setProgressSink(ProgressSink s);
        void emitProgress(const std::string& text, bool record_progress) const;

        bool startProbeJob(
            const std::filesystem::path& profile_path,
            const std::filesystem::path& capture_path,
            std::uint32_t progress_flags,
            std::string* error_out = nullptr);
        void stopProbeJob();
        bool probeJobActive() const;
        void emitProbeMarker(std::string_view marker, std::uint64_t value = 0) const;

        RunUntilHitResult runUntilBreakpointBlocking(uint32_t timeout_ms = 5000);
        RunUntilHitResult runUntilBreakpointFlexible(uint32_t timeout_ms,
            uint32_t vi_stall_ms = 0,
            bool watch_movie = true,
            uint32_t poll_ms = 0,
            uint32_t flags = {},
            ProgressSink sink = nullptr);

        void disableThrottle();
        void enableThrottle();

        uint32_t pickPollIntervalMs(uint32_t timeout_ms);
        static uint32_t pickPollIntervalMsForTimeLeft(uint32_t timeout_ms, uint32_t time_left_ms);

        void silenceStdOutInfo();
        void restoreStdOutInfo();

        // Convenience: query whether a DTM is currently being played back.
        bool isMoviePlaying() const;
        bool isMoviePlaybackEnded() const;
        uint64_t getCurrentMovieInputCount() const;
        bool startMoviePlayback(const std::string& dtm_path);
        bool endMoviePlaybackBlocking(uint32_t timeout_ms = 4000);
        bool setGCMemoryCardA(const std::string& raw_path);
        bool pauseEmulationBlocking(uint32_t timeout_ms = 1000);
        bool resumeEmulation();
        bool isEmulationPaused() const;

        bool startMovieRecording();
        void endMovieRecording(std::optional<std::string> movie_save_path = std::nullopt);


    private:

        // Cached pointer to Dolphin's singleton System.
        Core::System* m_system = nullptr;

        // Last-booted disc info (if we could read it before boot).
        std::optional<DiscInfo> m_disc_info;

        bool m_settingsLoaded = false;
        std::string m_last_save_state = "";
        bool m_ran_since_last_load = false;
        bool m_system_pad_is_inited = false;
        GCPadOverride m_pad{ 0 };
        std::string m_last_game_iso_path{""};
        WindowSystemInfo m_wsi;

        InputPlan m_plan;
        size_t m_cursor = 0;
        uint64_t m_input_playback_sequence = 0;

        bool waitForPausedCoreState(uint32_t timeout_ms, uint32_t poll_rate = 10);

        std::filesystem::path m_user_dir;
        std::filesystem::path m_qt_base_dir;
        bool m_imported_from_qt = false;
        bool m_visual_mode = false;
        void* m_external_render_widget_handle = nullptr;
        void sterilizeConfigs();
        void* m_render_window_handle = nullptr;
        bool createRenderSurfaceWindow();
        void destroyRenderSurfaceWindow();
        bool mutatePcBreakpoints(const char* label, const std::function<void()>& fn) const;

        ProgressSink m_progress_sink{};
        mutable std::mutex m_progress_sink_mutex;
        std::vector<MemoryWatchpointSpec> m_memory_watchpoints;
        std::uint64_t m_probe_epoch = 0;
    };

} // namespace savor
