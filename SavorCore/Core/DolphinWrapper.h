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

#include "Input/GCInputFrame.h"
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

        bool loadGame(
            const std::string& iso_path,
            bool boot_to_pause = false,
            std::optional<std::string> startup_savestate = std::nullopt);
        // Splits exact read-only playback around Dolphin's stopped-core
        // debugger boundary. Wrapper, controller, UI, user-directory, and
        // render-surface ownership remain live throughout.
        bool stopCoreForReadOnlyMovie(std::string* error_out = nullptr);
        bool startReadOnlyMovieFromStoppedCore(
            const std::string& dtm_path,
            std::optional<std::string>& startup_savestate_out,
            std::string* error_out = nullptr);
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
        struct InputPollReceipt {
            uint64_t epoch{0};
            uint32_t callback_count{0};
            uint32_t a_control_callback_count{0};
            GCInputFrame frame{};

            bool acknowledged() const noexcept
            {
                return epoch != 0 && callback_count != 0;
            }
        };
        uint64_t publishInputEpoch(const GCInputFrame& f);
        InputPollReceipt getInputPollReceipt() const;
        [[nodiscard]] bool isInputReady() const noexcept
        {
            return m_system_pad_is_inited;
        }
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

        bool startProbeJob(
            const std::filesystem::path& profile_path,
            const std::filesystem::path& capture_path,
            std::uint32_t progress_flags,
            std::vector<std::uint32_t> denied_profile_pcs,
            std::string* error_out = nullptr);
        void stopProbeJob();
        bool probeJobActive() const;
        void emitProbeMarker(std::string_view marker, std::uint64_t value = 0) const;

        void silenceStdOutInfo();
        void restoreStdOutInfo();

        bool setGCMemoryCardA(const std::string& raw_path);
        bool isEmulationPaused() const;

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

        uint64_t m_input_publication_epoch = 0;

        bool waitForPausedCoreState(uint32_t timeout_ms, uint32_t poll_rate = 10);
        // State-load bootstrap only. This is not a guest execution command
        // and is never exposed through IExecutionBackendPort.
        bool stepBootCoreForStateLoadBlocking(int timeout_ms);

        std::filesystem::path m_user_dir;
        std::filesystem::path m_qt_base_dir;
        bool m_imported_from_qt = false;
        bool m_visual_mode = false;
        void* m_external_render_widget_handle = nullptr;
        void sterilizeConfigs();
        void* m_render_window_handle = nullptr;
        bool createRenderSurfaceWindow();
        void destroyRenderSurfaceWindow();
    };

} // namespace savor
