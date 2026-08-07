#include "DolphinWrapper.h"
#include "Input/InputPlanFmt.h"

#include "UICommon/UICommon.h"      // SetUserDirectory, CreateDirectories
#include "Common/FileUtil.h"
#include <filesystem>
#include <fstream>
#include "../Utils/SafeEnv.h"
#include "../Utils/Log.h"
#include "../Utils/Time.h"
#include "../Runner/IPC/Wire.h"
#include "Memory/Soa/SoaAddrRegistry.h"
#include "Memory/MemView.h"

// Dolphin headers (adjust paths to your tree)
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "Core/State.h"
#include "Core/Movie.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/CPU.h"
#include "Core/HW/EXI/EXI.h"

#include "Core/ConfigManager.h"  // SConfig::Init/Shutdown/LoadSettings
#include "Core/Config/MainSettings.h"
#include "Common/Config/Config.h"
#include "Common/Event.h"
#include "Common/MsgHandler.h"

#include "Core/PowerPC/PowerPC.h"       // PowerPC::PowerPCManager, GetPPCState()

// DolphinWrapper.cpp
#include "InputCommon/ControllerInterface/ControllerInterface.h" // g_controller_interface
#include "InputCommon/InputConfig.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/GCPad.h"       // Pad::Initialize/Shutdown
#include "Core/HW/GCKeyboard.h"  // Keyboard::Initialize/Shutdown
#include "Core/HW/VideoInterface.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/Sram.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

#include "DiscIO/Enums.h"
#include "DiscIO/VolumeDisc.h"

#include "Core/VideoCommon/VideoBackendBase.h"  // WindowSystemInfo, WindowSystemType

#include <thread>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <mutex>
#include <optional>
#include <ranges>
#include <unordered_set>
#include <utility>

#include "Shims/StateBufferShim.h"
#include "../Tas/DtmFile.h"


using namespace std::chrono_literals;
using namespace std::chrono;
namespace fs = std::filesystem;
using addr::AddrKey;
using addr::DolphinAddr;

namespace {

    const char* DolphinAlertStyleName(Common::MsgType style)
    {
        switch (style) {
        case Common::MsgType::Information: return "information";
        case Common::MsgType::Question: return "question";
        case Common::MsgType::Warning: return "warning";
        case Common::MsgType::Critical: return "critical";
        default: return "unknown";
        }
    }

    bool HeadlessDolphinAlertHandler(const char* caption, const char* text, bool yes_no, Common::MsgType style)
    {
        SCLOGW("[dolphin-alert-suppressed] style=%s yes_no=%d caption=%s text=%s",
            DolphinAlertStyleName(style),
            yes_no ? 1 : 0,
            caption ? caption : "",
            text ? text : "");

        return style != Common::MsgType::Question;
    }

    void InstallHeadlessDolphinAlertHandler()
    {
        Common::RegisterMsgAlertHandler(HeadlessDolphinAlertHandler);
        Common::SetEnableAlert(true);
        Common::SetAbortOnPanicAlert(false);
    }

}

namespace savor {

    // --- small helpers ----------------------------------------------------------

    static inline const char* RegionToString(DiscIO::Region r) {
        switch (r) {
        case DiscIO::Region::NTSC_U: return "NTSC-U";
        case DiscIO::Region::NTSC_J: return "NTSC-J";
        case DiscIO::Region::PAL:    return "PAL";
        default:                     return "UNKNOWN";
        }
    }

    static WindowSystemInfo MakeHeadlessWSI() {
        WindowSystemInfo wsi{};
        wsi.type = WindowSystemType::Headless;
        return wsi;
    }

    static WindowSystemInfo MakeWindowsViewportWSI(void* hwnd)
    {
        WindowSystemInfo wsi{};
        wsi.type = WindowSystemType::Windows;
        wsi.render_window = hwnd;
        wsi.render_surface = hwnd;
        return wsi;
    }

#ifdef _WIN32
    static LRESULT CALLBACK SavorCoreViewportWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        return DefWindowProc(hwnd, msg, wparam, lparam);
    }
#endif

    // --- load settings helpers --------------------------------------------------

    static bool initPads() {
        Pad::Initialize();

        if (auto* ic = Pad::GetConfig()) {
            ic->LoadConfig();
            SCLOGI("[Pad] controllers=%d", ic->GetControllerCount());
        }

        Keyboard::Initialize();

        return true;
    }

    static void stopRealWiimoteScannerForHeadless(const char* phase)
    {
        SCLOGT("Stopping Dolphin real Wiimote scanner for headless wrapper: %s", phase ? phase : "");
        WiimoteReal::Shutdown();
    }

    static bool initWiimotesNone(bool stop_real_wiimote_scanner) {
        using namespace Wiimote;
        // Ensure module is initialized and configs loaded so controllers exist.
        Initialize(InitializeMode::DO_NOT_WAIT_FOR_WIIMOTES);
        if (stop_real_wiimote_scanner)
            stopRealWiimoteScannerForHeadless("initial initialize");
        if (auto* ic = GetConfig()) {
            ic->LoadConfig();
            SCLOGI("[Wii] controllers=%d", ic->GetControllerCount());
        }
        // Force all Wiimote sources to None, then re-init to materialize devices.
        Config::SetCurrent(Config::MAIN_CONNECT_WIIMOTES_FOR_CONTROLLER_INTERFACE, false);
        Shutdown();
        Initialize(InitializeMode::DO_NOT_WAIT_FOR_WIIMOTES);
        if (stop_real_wiimote_scanner)
            stopRealWiimoteScannerForHeadless("post-none reinitialize");
        if (auto* ic2 = GetConfig()) {
            ic2->LoadConfig();
            SCLOGI("[Wii] controllers(after None)=%d", ic2->GetControllerCount());
        }
        return true;
    }

    static bool loadDolphinGUISettings(WindowSystemInfo wsi, bool stop_real_wiimote_scanner) {
        SCLOGI("Loading Dolphin GUI settings");
        g_controller_interface.Initialize(wsi);
        initPads();
        initWiimotesNone(stop_real_wiimote_scanner);
        return true;
    }

    // --- class impl -------------------------------------------------------------

    DolphinWrapper::DolphinWrapper()
    {
        m_system = &Core::System::GetInstance();
        //log::Logger::get().open_file("savor.log", false);
        //log::Logger::get().set_levels(log::Level::Info, log::Level::Trace);
    }
    

    DolphinWrapper::~DolphinWrapper() {
        shutdownAll();
    }

    DolphinWrapper::ProgressSink DolphinWrapper::getProgressSink() const
    {
        std::scoped_lock lock(m_progress_sink_mutex);
        return m_progress_sink;
    }

    void DolphinWrapper::setProgressSink(ProgressSink sink)
    {
        std::scoped_lock lock(m_progress_sink_mutex);
        m_progress_sink = std::move(sink);
    }

    void DolphinWrapper::emitProgress(const std::string& text, bool record_progress) const
    {
        const auto sink = getProgressSink();
        if (sink)
            sink(text.c_str(), record_progress);
    }

    bool DolphinWrapper::startProbeJob(
        const std::filesystem::path& profile_path,
        const std::filesystem::path& capture_path,
        std::uint32_t progress_flags,
        std::vector<std::uint32_t> denied_profile_pcs,
        std::string* error_out)
    {
        (void)profile_path;
        (void)capture_path;
        (void)progress_flags;
        (void)denied_profile_pcs;
        if (error_out)
            *error_out =
                "hard cutover: production capture is unavailable until CaptureService";
        return false;
    }

    void DolphinWrapper::stopProbeJob()
    {
    }

    bool DolphinWrapper::probeJobActive() const
    {
        return false;
    }

    void DolphinWrapper::emitProbeMarker(std::string_view marker, std::uint64_t value) const
    {
        (void)marker;
        (void)value;
    }

    void DolphinWrapper::shutdownAll() {
        if (Core::IsRunning(*m_system))
            shutdownCore();
        destroyRenderSurfaceWindow();
        logger::Logger::get().close_file();
    }

    bool DolphinWrapper::isRunning() const noexcept {
        return Core::IsRunning(*m_system);
    }

    void DolphinWrapper::shutdownCore() {
        if (Core::IsRunning(*m_system))
            Core::Stop(*m_system);

        for (int i = 0; i < 50 && Core::IsRunning(*m_system); ++i) {
            Core::HostDispatchJobs(*m_system);
            std::this_thread::sleep_until(steady_clock::now() + milliseconds(1));
        }

        Wiimote::Shutdown();
        Keyboard::Shutdown();
        Pad::Shutdown();
        g_controller_interface.Shutdown();
        Core::Shutdown(*m_system);
        UICommon::Shutdown();
    }

    void DolphinWrapper::stop() {
        if (Core::IsRunning(*m_system))
            Core::Stop(*m_system);
    }

    bool DolphinWrapper::loadGame(
        const std::string& iso_path,
        bool boot_to_pause,
        std::optional<std::string> startup_savestate)
    {
        if (!m_imported_from_qt) {
            SCLOGE("Must import sys folder from DolphinQT before loading a game. (Best to use Dolphin ver. 2506a");
            return false;
        }
        
        if (Core::IsRunning(*m_system))
        {
            shutdownCore();
        }

        const void* render_handle = nullptr;
        if (m_visual_mode) {
            if (m_external_render_widget_handle != nullptr) {
                m_render_window_handle = m_external_render_widget_handle;
            } else if (!createRenderSurfaceWindow()) {
                SCLOGE("Failed to create render surface window for visual mode");
                return false;
            }
            render_handle = m_render_window_handle;
        }
        const WindowSystemInfo wsi = m_visual_mode
            ? MakeWindowsViewportWSI(const_cast<void*>(render_handle))
            : MakeHeadlessWSI();

        m_wsi = wsi;

        SetUserDirectory(m_user_dir);
        sterilizeConfigs();

        m_system_pad_is_inited = loadDolphinGUISettings(wsi, !m_visual_mode);
        // Production sessions begin at an authoritative CPU-idle boundary.
        // Dolphin consumes this on the CPU thread before entering its first
        // guest run loop.
        SConfig::GetInstance().bBootToPause = boot_to_pause;

        auto volume = DiscIO::CreateVolume(iso_path);
        if (!volume)
            return false;
        m_disc_info = DiscInfo{ volume->GetGameID(), RegionToString(volume->GetRegion()) };

        auto boot = BootParameters::GenerateFromFile(iso_path);
        if (startup_savestate.has_value())
        {
            boot->boot_session_data.SetSavestateData(
                std::move(startup_savestate),
                DeleteSavestateAfterBoot::No);
        }
        if (!BootManager::BootCore(*m_system, std::move(boot), wsi))
            return false;

        m_last_game_iso_path = iso_path;

        auto deadline = std::chrono::steady_clock::now() + 20s;
        while (std::chrono::steady_clock::now() < deadline) {
            Core::HostDispatchJobs(*m_system);
            if (Core::IsRunning(*m_system) &&
                (!boot_to_pause ||
                    Core::GetState(*m_system) == Core::State::Paused)) {
                break;
            }
            std::this_thread::sleep_until(steady_clock::now() + milliseconds(1));
        }

        return Core::IsRunning(*m_system) &&
            (!boot_to_pause ||
                Core::GetState(*m_system) == Core::State::Paused);
    }

    bool DolphinWrapper::stopCoreForReadOnlyMovie(std::string* error_out)
    {
        const auto fail = [&](std::string message) {
            if (error_out)
                *error_out = std::move(message);
            return false;
        };
        if (!m_imported_from_qt || m_last_game_iso_path.empty())
            return fail("Dolphin core stop requires an initialized wrapper and current game");

        if (Core::IsRunning(*m_system))
        {
            if (Core::GetState(*m_system) == Core::State::Paused)
                Core::SetState(*m_system, Core::State::Running);
            const auto running_deadline = steady_clock::now() + 5s;
            while (Core::GetState(*m_system) != Core::State::Running &&
                   steady_clock::now() < running_deadline)
            {
                Core::HostDispatchJobs(*m_system);
                std::this_thread::sleep_until(
                    steady_clock::now() + milliseconds(1));
            }
            Core::Stop(*m_system);
        }
        const auto stop_deadline = steady_clock::now() + 20s;
        while (!Core::IsUninitialized(*m_system) &&
               steady_clock::now() < stop_deadline)
        {
            Core::HostDispatchJobs(*m_system);
            std::this_thread::sleep_until(
                steady_clock::now() + milliseconds(1));
        }
        if (!Core::IsUninitialized(*m_system))
            return fail("Dolphin guest core did not reach the uninitialized state");

        return true;
    }

    bool DolphinWrapper::startReadOnlyMovieFromStoppedCore(
        const std::string& dtm_path,
        std::optional<std::string>& startup_savestate_out,
        std::string* error_out)
    {
        const auto fail = [&](std::string message) {
            if (error_out)
                *error_out = std::move(message);
            return false;
        };
        if (!m_imported_from_qt || m_last_game_iso_path.empty())
            return fail("Dolphin movie start requires an initialized wrapper and current game");
        if (dtm_path.empty())
            return fail("Dolphin movie start requires an exact DTM path");
        if (!Core::IsUninitialized(*m_system))
            return fail("Dolphin movie start requires an uninitialized guest core");

        auto& movie = m_system->GetMovie();
        movie.SetReadOnly(true);
        if (movie.IsMovieActive())
            movie.EndPlayInput(false);
        std::optional<std::string> startup_savestate;
        if (!movie.PlayInput(dtm_path, &startup_savestate))
            return fail("Dolphin rejected the prepared read-only DTM");

        SConfig::GetInstance().bBootToPause = true;
        Config::SetCurrent(Config::MAIN_ENABLE_DEBUGGING, true);
        auto boot = BootParameters::GenerateFromFile(m_last_game_iso_path);
        if (startup_savestate)
        {
            boot->boot_session_data.SetSavestateData(
                *startup_savestate,
                DeleteSavestateAfterBoot::No);
        }
        if (!BootManager::BootCore(*m_system, std::move(boot), m_wsi))
        {
            movie.EndPlayInput(false);
            return fail("Dolphin failed to boot the current game for read-only playback");
        }

        const auto boot_deadline = steady_clock::now() + 20s;
        while (steady_clock::now() < boot_deadline)
        {
            Core::HostDispatchJobs(*m_system);
            if (Core::IsRunning(*m_system) &&
                Core::GetState(*m_system) == Core::State::Paused)
            {
                startup_savestate_out = std::move(startup_savestate);
                return true;
            }
            std::this_thread::sleep_until(
                steady_clock::now() + milliseconds(1));
        }
        if (movie.IsMovieActive())
            movie.EndPlayInput(false);
        return fail("Dolphin movie core restart did not reach a paused boundary");
    }

    bool DolphinWrapper::runOnCpuThread(const std::function<void()>& fn, const bool waitForCompletion) const
    {
        if (!Core::IsRunning(*m_system))
            return false;

        const auto start = std::chrono::steady_clock::now();
        SCLOGD("[DW] runOnCpuThread begin wait=%d running=%d state=%d",
            waitForCompletion ? 1 : 0, Core::IsRunning(*m_system) ? 1 : 0, (int)Core::GetState(*m_system));

        std::atomic<bool> done{ false };
        SCLOGD("[DW] runOnCpuThread dispatch begin");
        Core::RunOnCPUThread(*m_system, [&] {
            fn();
            done = true;
            }, waitForCompletion);
        SCLOGD("[DW] runOnCpuThread dispatch returned done=%d", done ? 1 : 0);

        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!done && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_until(steady_clock::now() + milliseconds(1));

        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        SCLOGD("[DW] runOnCpuThread end   done=%d ms=%lld", done ? 1 : 0, (long long)ms);

        return done.load();
    }

    uint32_t DolphinWrapper::getRegister(uint8_t reg)
    {
        if (Core::GetState(*m_system) == Core::State::Paused)
            return m_system->GetPowerPC().GetPPCState().gpr[reg];

        uint32_t value = 0;
        runOnCpuThread([&] {
            value = m_system->GetPowerPC().GetPPCState().gpr[reg];
            }, true);
        return value;
    }

    uint32_t DolphinWrapper::getLinkRegister()
    {
        if (Core::GetState(*m_system) == Core::State::Paused)
            return LR(m_system->GetPowerPC().GetPPCState());

        uint32_t value = 0;
        runOnCpuThread([&] {
            value = LR(m_system->GetPowerPC().GetPPCState());
            }, true);
        return value;
    }

    std::vector<DolphinWrapper::PowerPcStackFrame> DolphinWrapper::getPowerPcCallStack(uint32_t max_depth)
    {
        std::vector<PowerPcStackFrame> frames;
        if (max_depth == 0) {
            return frames;
        }

        std::unordered_set<uint32_t> seen_sps;
        uint32_t sp = getRegister(1);
        for (uint32_t depth = 0; depth < max_depth && sp != 0; ++depth) {
            if (!seen_sps.insert(sp).second) {
                break;
            }

            uint32_t next_sp = 0;
            uint32_t return_pc = 0;
            if (!readU32(sp, next_sp) || !readU32(sp + 4u, return_pc)) {
                break;
            }

            frames.push_back(PowerPcStackFrame{
                .depth = depth,
                .sp = sp,
                .next_sp = next_sp,
                .return_pc = return_pc,
                .callsite_pc = return_pc >= 4u ? return_pc - 4u : 0u,
            });

            if (next_sp == 0 || next_sp == sp || (next_sp & 0x3u) != 0) {
                break;
            }
            sp = next_sp;
        }
        return frames;
    }

    uint32_t DolphinWrapper::getPC()
    {
        if (Core::GetState(*m_system) == Core::State::Paused)
            return m_system->GetPowerPC().GetPPCState().pc;
        
        uint32_t pc = 0;
        runOnCpuThread([&] {
            pc = m_system->GetPowerPC().GetPPCState().pc;
            }, true );
        return pc;
    }

    uint64_t DolphinWrapper::getTBR()
    {
        if (Core::GetState(*m_system) == Core::State::Paused)
            return m_system->GetPowerPC().ReadFullTimeBaseValue();
        
        uint64_t tbr = 0;
        runOnCpuThread([&] {
            tbr = m_system->GetPowerPC().ReadFullTimeBaseValue();
            }, true);
        return tbr;
    }

    uint32_t DolphinWrapper::getConfigRTC(bool offset)
    {
        uint32_t rtc = Config::Get(Config::MAIN_CUSTOM_RTC_VALUE);
        if (offset) rtc = rtc - savor::tas::base_sec;
        return rtc;
    }

    uint32_t DolphinWrapper::getEmulatedTime()
    {
        return ExpansionInterface::CEXIIPL::GetEmulatedTime(*m_system, ExpansionInterface::CEXIIPL::GC_EPOCH);
    }

    std::string DolphinWrapper::getCurrentSctFileTag() const
    {
        uint32_t num = 0;
        uint8_t  ch = 0;

        // Try both reads; if either fails, return empty.
        if (!readByKey(addr::core::SCT_FILE_NUM, num, false)) return {};
        if (!readByKey(addr::core::SCT_FILE_LTTR, ch, false))  return {};

        // Enforce expected ranges: number 1..600; letter must be a printable ASCII.
        if (num < 1 || num > 600) return {};
        if (ch < 0x20 || ch > 0x7E) return {};

        char buf[32];
        std::snprintf(buf, sizeof(buf), "SCT_FILE:%03u%c", num, static_cast<char>(ch));
        return std::string(buf);
    }

    std::string DolphinWrapper::getCurrentSctSection() const
    {
        
        uint32_t start = 0;
        uint32_t  pos = 0;

        // Try both reads; if either fails, return empty.
        if (!readByKey(addr::core::SCT_FIRST_INST, start, false)) return {};
        if (!readByKey(addr::core::SCT_CURRENT_INST, pos, false))  return {};

        if (start == 0 || pos == 0) return {};

        uint32_t offset = pos - start;

        uint32_t section_count = 0;
        uint32_t p_index = 0;

        // Try both reads; if either fails, return empty.
        if (!readByKey(addr::core::SCT_SECTION_COUNT, section_count, false)) return {};
        if (!readByKey(addr::core::SCT_INDEX_BUFFER, p_index, false))  return {};

        std::string name{"..."};
        uint32_t section_offset = 0;
        for (uint32_t i = 0; i + 1 < section_count; i++) {
            uint32_t next_offset = 0;
            if (!readU32(p_index + ((i + 1) * 0x14), next_offset)) return {};
            if (next_offset >= offset) 
            {
                if (!readU32(p_index + (i * 0x14), section_offset)) return {};
                if (!getMem1RangeRaw(name, p_index + (i * 0x14) + 4, 0x10)) return {};
                break;
            }
        }

        offset -= section_offset;

        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s:0x%X", name.c_str(), offset);
        return std::string(buf);
    }

    bool savor::DolphinWrapper::getMem1(std::string& out) const
    {
        if (!isRunning()) return false;
        out.resize(0x01800000u);

        auto copier = [&] {
            auto& mem = m_system->GetMemory();
            // CopyFromEmu(dst, VA, size) - big block copy of MEM1 starting at 0x80000000
            mem.CopyFromEmu(out.data(), 0x80000000u, out.size());
            };

        if (Core::GetState(*m_system) == Core::State::Paused)
        {
            copier();
            return true;
        }
        else
        {
            return runOnCpuThread([&] { copier(); }, true);
        }
    }

    bool DolphinWrapper::getMem1RangeRaw(std::string& out, uint32_t va, uint32_t size) const
    {
        if (!isRunning()) return false;
        out.resize(size);

        auto copier = [&] {
            auto& mem = m_system->GetMemory();
            // CopyFromEmu(dst, VA, size) - big block copy of MEM1 starting at 0x80000000
            mem.CopyFromEmu(out.data(), va, size);
            };

        if (Core::GetState(*m_system) == Core::State::Paused)
        {
            copier();
            return true;
        }
        else
        {
            return runOnCpuThread([&] { copier(); }, true);
        }
    }


    bool DolphinWrapper::loadSavestate(const std::string& state_path)
    {
        if (!Core::IsRunning(*m_system))
            return false;

        const Core::State entry_state = Core::GetState(*m_system);
        const bool restore_paused =
            entry_state == Core::State::Paused;
        const bool debugging_enabled =
            Config::Get(Config::MAIN_ENABLE_DEBUGGING);
        SCLOGD(
            "[DW] loadSavestateFromFile begin path=%s is_cpu_thread=%d "
            "state=%d restore_paused=%d",
            state_path.c_str(),
            Core::IsCPUThread(),
            static_cast<int>(entry_state),
            restore_paused ? 1 : 0);

        uint32_t pc_before = getPC();
        uint64_t tbr_before = getTBR();

        if (restore_paused && tbr_before == 0 &&
            !stepBootCoreForStateLoadBlocking(5000))
        {
            SCLOGW(
                "[DW] loadSavestate could not step the paused boot core "
                "before state replacement");
            return false;
        }

        // State::LoadAs rejects a paused core before it schedules its own
        // synchronous CPU callback. Briefly make the core runnable and invoke
        // LoadAs from this host thread so that preflight sees Running. Debug
        // mode itself pauses at the boot PC, so suppress that gate until
        // Dolphin's after-load callback restores both debugging and Paused
        // before the CPU callback returns.
        std::atomic<bool> paused_restore_completed{false};
        if (restore_paused)
        {
            if (debugging_enabled)
            {
                Config::SetCurrent(
                    Config::MAIN_ENABLE_DEBUGGING,
                    false);
            }
            Core::SetState(*m_system, Core::State::Running);
            State::SetOnAfterLoadCallback([&] {
                if (debugging_enabled)
                {
                    Config::SetCurrent(
                        Config::MAIN_ENABLE_DEBUGGING,
                        true);
                }
                Core::SetState(*m_system, Core::State::Paused);
                paused_restore_completed.store(
                    true,
                    std::memory_order_release);
            });
        }
        State::LoadAs(*m_system, state_path);
        const bool scheduled =
            !restore_paused ||
            paused_restore_completed.load(std::memory_order_acquire);
        if (restore_paused)
            State::SetOnAfterLoadCallback({});

        // (you already do pc_before/tbr_before)
        SCLOGD("[DW] loadSavestate scheduled=%d", scheduled ? 1 : 0);

        if (!scheduled) {
            if (restore_paused)
            {
                if (debugging_enabled)
                {
                    Config::SetCurrent(
                        Config::MAIN_ENABLE_DEBUGGING,
                        true);
                }
                Core::SetState(*m_system, Core::State::Paused);
            }
            return false;
        }

        // Right before the final return
        const uint32_t pc_after = getPC();
        const uint64_t tbr_after = getTBR();

        SCLOGD("[DW] loadSavestate end state=%d pc:%08X->%08X tbr:%016llX->%016llX movie_state_restored",
            (int)Core::GetState(*m_system), pc_before, pc_after,
            (unsigned long long)tbr_before, (unsigned long long)tbr_after);

        if (Core::IsRunning(*m_system) && (pc_before != pc_after || tbr_before != tbr_after || state_path._Equal(m_last_save_state))) {
            m_last_save_state = state_path;
            return true;
        }
        else {
            SCLOGW(
                "[DW] loadSavestate produced no observable state replacement "
                "scheduled=%d entry_state=%d exit_state=%d pc:%08X->%08X "
                "tbr:%016llX->%016llX",
                scheduled ? 1 : 0,
                static_cast<int>(entry_state),
                static_cast<int>(Core::GetState(*m_system)),
                pc_before,
                pc_after,
                static_cast<unsigned long long>(tbr_before),
                static_cast<unsigned long long>(tbr_after));
            return false;
        }
    }

    bool DolphinWrapper::saveSavestateBlocking(const std::string& path)
    {
        const auto state = Core::GetState(*m_system);
        if (!Core::IsRunning(*m_system) && state != Core::State::Paused) {
            SCLOGW("[DW] saveSavestateBlocking rejected path=%s state=%d running=0", path.c_str(), static_cast<int>(state));
            return false;
        }
        if (const auto parent = std::filesystem::path(path).parent_path(); !parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                SCLOGW("[DW] saveSavestateBlocking create_directories failed path=%s error=%s", parent.string().c_str(), ec.message().c_str());
                return false;
            }
        }
        if (Core::GetState(*m_system) != Core::State::Paused)
            Core::SetState(*m_system, Core::State::Paused, false, false);
        while (Core::GetState(*m_system) != Core::State::Paused)
            std::this_thread::sleep_for(milliseconds(10));
        SCLOGI("[DW] saveSavestateBlocking begin path=%s state=%d running=%d", path.c_str(), static_cast<int>(Core::GetState(*m_system)), Core::IsRunning(*m_system) ? 1 : 0);
        State::SaveAs(*m_system, path, true);
        SCLOGI("[DW] saveSavestateBlocking end path=%s exists=%d", path.c_str(), std::filesystem::exists(path) ? 1 : 0);
        return true;
    }

    bool DolphinWrapper::saveScreenshotBlocking(const std::string& path, uint32_t timeout_ms)
    {
        if (!Core::IsRunning(*m_system)) {
            SCLOGW("[DW] saveScreenshotBlocking rejected path=%s running=0", path.c_str());
            return false;
        }

        const auto output_path = std::filesystem::path(path);
        const auto parent = output_path.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                SCLOGW("[DW] saveScreenshotBlocking create_directories failed path=%s error=%s",
                    parent.string().c_str(),
                    ec.message().c_str());
                return false;
            }
        }

        std::error_code remove_ec;
        std::filesystem::remove(output_path, remove_ec);

        SCLOGI("[DW] saveScreenshotBlocking begin path=%s visual=%d state=%d",
            path.c_str(),
            m_visual_mode ? 1 : 0,
            static_cast<int>(Core::GetState(*m_system)));
        Core::SaveScreenShot(path);

        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeout_ms == 0 ? 3000 : timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            Core::HostDispatchJobs(*m_system);
            std::error_code ec;
            if (std::filesystem::exists(output_path, ec)) {
                const auto size = std::filesystem::file_size(output_path, ec);
                if (!ec && size > 0) {
                    SCLOGI("[DW] saveScreenshotBlocking end path=%s exists=1 size=%llu",
                        path.c_str(),
                        static_cast<unsigned long long>(size));
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        std::error_code ec;
        const bool exists = std::filesystem::exists(output_path, ec);
        SCLOGW("[DW] saveScreenshotBlocking timeout path=%s exists=%d",
            path.c_str(),
            exists ? 1 : 0);
        return exists;
    }

    bool DolphinWrapper::saveStateToBuffer(Common::UniqueBuffer<u8>& buffer)
    {
        State::SaveToBuffer(*m_system, buffer);
        return true;
    }

    bool DolphinWrapper::loadStateFromBuffer(Common::UniqueBuffer<u8>& buf)
    {
        SCLOGD("[DW] loadStateFromBuffer begin state=%d is_cpu_thread=%d",
            (int)Core::GetState(*m_system), Core::IsCPUThread());
        const uint32_t pc_before = getPC();
        const uint64_t tbr_before = getTBR();
        
        SAVOR_LoadFromBufferShim(*m_system, buf);

        const uint32_t pc_after = getPC();
        const uint64_t tbr_after = getTBR();
        SCLOGD("[DW] loadStateFromBuffer end   state=%d pc:%08X->%08X tbr:%016llX->%016llX",
            (int)Core::GetState(*m_system), pc_before, pc_after,
            (unsigned long long)tbr_before, (unsigned long long)tbr_after);
        return true;
    }

    bool DolphinWrapper::setGCMemoryCardA(const std::string& raw_path)
    {
        // We avoid Dolphin source changes: copy the provided RAW to the
        // standard per-region filenames so the current game will pick it up.
        // MemoryCardA.<REG>.raw is the path Dolphin expects for "Memory Card" mode.
        // (Documented widely in user guides/bug threads.) 
        // USA/JAP/PAL cover GC regions.
        try {
            const fs::path gc_dir = fs::path(m_user_dir) / "GC";
            fs::create_directories(gc_dir);
            const fs::path memcard_path = gc_dir / "MemoryCardA.USA.raw";

            if (raw_path.empty())
            {
                if (!fs::exists(memcard_path))
                {
                    auto memcard = Memcard::GCMemcard::Create(
                        memcard_path.string(),           // path
                        CardFlashId{},                   // new card flash id
                        16,                              // size (mbits)
                        false,                           // is Shift-JIS
                        0,                               // rtc bias
                        0,                               // sram language
                        Common::Timer::GetLocalTimeSinceJan1970() - ExpansionInterface::CEXIIPL::GC_EPOCH);
                    memcard->Save();

                    SCLOGI(
                        "[MemCard] Created RAW at %s",
                        memcard_path.string().c_str());
                }
            }
            else 
            {
                fs::copy_file(raw_path, memcard_path, fs::copy_options::overwrite_existing);
                SCLOGI(
                    "[MemCard] Copied RAW to %s",
                    gc_dir.string().c_str());
            }

            SCLOGT(
                "[MemCard] Setting memcard to %s",
                memcard_path.string().c_str());
            Config::SetCurrent(Config::MAIN_MEMCARD_A_PATH, memcard_path.string());
            return true;
        }
        catch (...) {
            return false;
        }
    }

    void DolphinWrapper::applyNextInputFrame() {
        if (!m_system_pad_is_inited) return;

        if (m_cursor < m_plan.size()) {
            const auto& f = m_plan[m_cursor];
            SCLOGD("[INP] next #%zu btn=%04X main=(%u,%u) c=(%u,%u) trig=(%u,%u)",
                m_cursor, f.buttons, f.main_x, f.main_y, f.c_x, f.c_y, f.trig_l, f.trig_r);
            m_pad.setFrame(m_plan[m_cursor++]);
        }
        else {
            SCLOGD("[INP] next <neutral>");
            m_pad.setFrame(GCPadOverride::NeutralFrame());
        }
    }

    void DolphinWrapper::setInput(const GCInputFrame& f)
    {
        if (!m_system_pad_is_inited) return;

        m_pad.setFrame(f);

        SCLOGD("[INP] set btn=%04X main=(%u,%u) c=(%u,%u) trig=(%u,%u)",
            f.buttons, f.main_x, f.main_y, f.c_x, f.c_y, f.trig_l, f.trig_r);
    }

    uint64_t DolphinWrapper::publishInputEpoch(const GCInputFrame& f)
    {
        if (!m_system_pad_is_inited) return 0;

        uint64_t epoch = ++m_input_playback_sequence;
        if (epoch == 0) epoch = ++m_input_playback_sequence;
        m_pad.publishPlaybackFrame(epoch, 0, f);
        SCLOGD("[INP] publish epoch=%llu btn=%04X main=(%u,%u) c=(%u,%u) trig=(%u,%u)",
            static_cast<unsigned long long>(epoch),
            f.buttons, f.main_x, f.main_y, f.c_x, f.c_y, f.trig_l, f.trig_r);
        return epoch;
    }

    DolphinWrapper::InputPollReceipt DolphinWrapper::getInputPollReceipt() const
    {
        const auto stats = m_pad.getPollStats();
        return InputPollReceipt{
            .epoch = stats.sequence,
            .callback_count = stats.callback_count,
            .frame = stats.frame,
        };
    }

    DolphinWrapper::InputTapePlaybackResult DolphinWrapper::playInputTapeBlocking(
        const InputPlan& plan,
        const InputTapePlaybackOptions& options)
    {
        (void)plan;
        (void)options;
        SCLOGE(
            "[input-tape] hard cutover: direct tape execution is disconnected; "
            "use InputArbiter and ExecutionEngine");
        return {};
#if 0
        InputTapePlaybackResult result{};
        if (!m_system_pad_is_inited) {
            result.failed_index = 0;
            SCLOGDX(SC_TAGS("input"), "[input-tape] label=%s not_started reason=pad_not_initialized", options.label);
            return result;
        }

        const GCInputFrame neutral = GCPadOverride::NeutralFrame();
        auto is_neutral = [&neutral](const GCInputFrame& f) {
            return f == neutral;
            };

        InputPlan playback_plan;
        playback_plan.reserve(options.safe_mode ? plan.size() * 2u : plan.size());
        if (options.safe_mode) {
            bool previous_was_active = false;
            for (const auto& frame : plan) {
                const bool active = !is_neutral(frame);
                playback_plan.push_back(frame);
                if (active) {
                    playback_plan.push_back(frame);
                }
                else if (previous_was_active) {
                    playback_plan.push_back(frame);
                }
                previous_was_active = active;
            }
        }
        else {
            playback_plan = plan;
        }

        SCLOGDX(SC_TAGS("input"),
            "[input-tape] label=%s begin source_frames=%zu playback_frames=%zu safe_mode=%u max_unacked_replays=%u",
            options.label,
            plan.size(),
            playback_plan.size(),
            options.safe_mode ? 1u : 0u,
            options.max_unacked_replays);

        result.attempted_frames.reserve(playback_plan.size());
        result.vi_durations.reserve(playback_plan.size());

        for (uint32_t idx = 0; idx < playback_plan.size(); ++idx) {
            const GCInputFrame& frame = playback_plan[idx];
            bool acknowledged = false;
            for (uint32_t replay = 0; replay <= options.max_unacked_replays; ++replay) {
                const uint64_t sequence = ++m_input_playback_sequence;
                const uint32_t vi_before = static_cast<uint32_t>(getViFieldCountApprox() & 0xFFFFFFFFull);
                const uint32_t pc_before = getPC();
                m_pad.publishPlaybackFrame(sequence, idx, frame);

                SCLOGDX(SC_TAGS("input"),
                    "[input-tape] label=%s publish seq=%llu idx=%u replay=%u vi_before=%u pc_before=%08X frame=%s",
                    options.label,
                    static_cast<unsigned long long>(sequence),
                    idx,
                    replay,
                    vi_before,
                    pc_before,
                    DescribeFrameCompact(frame).c_str());

                const bool step_ok = stepOneFrameBlocking();
                const auto stats = m_pad.getPollStats();
                const uint32_t vi_after = static_cast<uint32_t>(getViFieldCountApprox() & 0xFFFFFFFFull);
                const uint32_t pc_after = getPC();
                const uint32_t vi_delta = (vi_after >= vi_before) ? (vi_after - vi_before) : 0u;
                result.attempted_frames.push_back(frame);
                result.vi_durations.push_back(vi_delta);

                SCLOGDX(SC_TAGS("input"),
                    "[input-tape] label=%s ack seq=%llu idx=%u replay=%u step_ok=%u callbacks=%u vi_after=%u vi_delta=%u pc_after=%08X",
                    options.label,
                    static_cast<unsigned long long>(sequence),
                    idx,
                    replay,
                    step_ok ? 1u : 0u,
                    stats.callback_count,
                    vi_after,
                    vi_delta,
                    pc_after);

                if (!step_ok) {
                    result.failed_index = idx;
                    SCLOGDX(SC_TAGS("input"),
                        "[input-tape] label=%s failed seq=%llu idx=%u reason=step_failed",
                        options.label,
                        static_cast<unsigned long long>(sequence),
                        idx);
                    return result;
                }

                if (stats.sequence == sequence && stats.callback_count > 0) {
                    acknowledged = true;
                    break;
                }

                ++result.unacked_count;
                SCLOGDX(SC_TAGS("input"),
                    "[input-tape] label=%s unacked seq=%llu idx=%u replay=%u callbacks=%u",
                    options.label,
                    static_cast<unsigned long long>(sequence),
                    idx,
                    replay,
                    stats.callback_count);
            }

            if (!acknowledged) {
                result.failed_index = idx;
                SCLOGDX(SC_TAGS("input"),
                    "[input-tape] label=%s failed idx=%u reason=unacked_after_replays",
                    options.label,
                    idx);
                return result;
            }
        }

        result.ok = true;
        SCLOGDX(SC_TAGS("input"),
            "[input-tape] label=%s complete attempted_frames=%zu unacked_replays=%u",
            options.label,
            result.attempted_frames.size(),
            result.unacked_count);
        return result;
#endif
    }

    // -- Frame Advancing --------------------------------

    bool DolphinWrapper::stepBootCoreForStateLoadBlocking(int timeout_ms)
    {
        if (!m_system || !Core::IsRunning(*m_system))
            return false;

        Common::Event sync_event;
        auto& power_pc = m_system->GetPowerPC();
        const PowerPC::CoreMode old_mode = power_pc.GetMode();
        power_pc.SetMode(PowerPC::CoreMode::Interpreter);
        m_system->GetCPU().StepOpcode(&sync_event);
        const bool completed = sync_event.WaitFor(
            std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 20));
        power_pc.SetMode(old_mode);
        return completed;
    }

    bool DolphinWrapper::stepOneFrameBlocking(int timeout_ms)
    {
        (void)timeout_ms;
        SCLOGE(
            "[DW/run] hard cutover: direct frame stepping is disconnected; "
            "use ExecutionEngine");
        return false;
    }

    bool DolphinWrapper::pauseEmulationBlocking(uint32_t timeout_ms)
    {
        (void)timeout_ms;
        SCLOGE(
            "[DW/run] hard cutover: direct pause is disconnected; "
            "use ExecutionEngine");
        return false;
    }

    bool DolphinWrapper::resumeEmulation()
    {
        SCLOGE(
            "[DW/run] hard cutover: direct resume is disconnected; "
            "use ExecutionEngine");
        return false;
    }

    bool DolphinWrapper::isEmulationPaused() const
    {
        return m_system && Core::GetState(*m_system) == Core::State::Paused;
    }

    static uint64_t g_vi_ticks_baseline = 0;

    void DolphinWrapper::resetViCounterBaseline()
    {
        auto& sys = Core::System::GetInstance();
        g_vi_ticks_baseline = sys.GetCoreTiming().GetTicks();
    }

    uint64_t DolphinWrapper::getViFieldCountApprox() const
    {
        auto& sys = Core::System::GetInstance();
        const uint64_t now_ticks = sys.GetCoreTiming().GetTicks();

        auto& vi = sys.GetVideoInterface();
        const uint32_t ticks_per_field = vi.GetTicksPerField();
        if (ticks_per_field == 0)
            return 0;

        return now_ticks / ticks_per_field;
    }

    uint64_t DolphinWrapper::getViFieldCountApproxFromBaseline() const
    {
        auto& sys = Core::System::GetInstance();
        const uint64_t now_ticks = sys.GetCoreTiming().GetTicks();
        const uint64_t dt = (now_ticks >= g_vi_ticks_baseline) ? (now_ticks - g_vi_ticks_baseline) : 0;

        auto& vi = sys.GetVideoInterface();
        const uint32_t ticks_per_field = vi.GetTicksPerField();
        if (ticks_per_field == 0)
            return 0;

        return dt / ticks_per_field;
    }

    uint64_t DolphinWrapper::getFrameCountApprox(bool interlaced) const
    {
        const uint64_t fields = getViFieldCountApproxFromBaseline();
        return interlaced ? (fields / 2) : fields;
    }

    static inline void write_all(const fs::path& p, const std::string& s) {
        fs::create_directories(p.parent_path());
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        ofs.write(s.data(), (std::streamsize)s.size());
    }

    static inline bool copy_tree(const fs::path& src, const fs::path& dst, std::string* err = nullptr) {
        if (!fs::exists(src)) { if (err) *err = "Missing source: " + src.string(); return false; }
        std::error_code ec;
        fs::create_directories(dst, ec);
        ec.clear();
        fs::copy(src, dst,
            fs::copy_options::recursive |
            fs::copy_options::overwrite_existing |
            fs::copy_options::copy_symlinks,
            ec);
        if (ec) { if (err) *err = "Copy failed: " + ec.message(); return false; }
        return true;
    }

    static inline bool require_exists_dir(const fs::path& p, const char* what, std::string* err) {
        if (!fs::exists(p) || !fs::is_directory(p)) {
            if (err) *err = std::string("Missing ") + what + ": " + p.string();
            return false;
        }
        return true;
    }

    // --- API -------------------------------------------------------------------

    bool DolphinWrapper::SetUserDirectory(const fs::path& user_dir)
    {
        m_user_dir = user_dir;
        try {
            InstallHeadlessDolphinAlertHandler();
            fs::create_directories(m_user_dir / "Config");
            UICommon::SetUserDirectory(m_user_dir.string());
            UICommon::CreateDirectories();
            UICommon::Init();
            SConfig::Init();
            SConfig::GetInstance().LoadSettings();
            return true;
        }
        catch (...) { return false; }
    }

    void DolphinWrapper::ConfigurePortsStandardPadP1()
    {
        Config::SetCurrent(Config::GetInfoForSIDevice(0), SerialInterface::SIDevices::SIDEVICE_GC_CONTROLLER);
        Config::SetCurrent(Config::GetInfoForSIDevice(1), SerialInterface::SIDevices::SIDEVICE_NONE);
        Config::SetCurrent(Config::GetInfoForSIDevice(2), SerialInterface::SIDevices::SIDEVICE_NONE);
        Config::SetCurrent(Config::GetInfoForSIDevice(3), SerialInterface::SIDevices::SIDEVICE_NONE);
        if (m_system_pad_is_inited)   
        {
            Pad::Shutdown();
            Pad::Initialize();
        }
        else {
            Pad::Initialize();
            m_system_pad_is_inited = true;
        }
        m_pad.install();
    }

    bool DolphinWrapper::QueryPadStatus(int port, GCPadStatus* out) const
    {
        if (!out) return false;
        *out = Pad::GetStatus(port);
        return true;
    }


    bool DolphinWrapper::SetDolphinQtBaseDir(const fs::path& dolphin_base_dir, std::string* error_out)
    {
        std::string err;
        if (!require_exists_dir(dolphin_base_dir, "DolphinQt base", &err)) { if (error_out) *error_out = err; return false; }

        if (!fs::exists(dolphin_base_dir / "portable.txt")) {
            if (error_out) *error_out = "portable.txt not found in base: " + dolphin_base_dir.string();
            return false;
        }

        const fs::path sys = dolphin_base_dir / "Sys";
        const fs::path user = dolphin_base_dir / "User";
        if (!require_exists_dir(sys, "Sys", &err)) { if (error_out) *error_out = err; return false; }
        if (!require_exists_dir(user, "User", &err)) { if (error_out) *error_out = err; return false; }

        m_qt_base_dir = dolphin_base_dir;
        m_imported_from_qt = false;
        return true;
    }

    bool DolphinWrapper::SyncFromDolphinQtBase(bool force, std::string* error_out)
    {
        if (m_qt_base_dir.empty()) {
            if (error_out) *error_out = "DolphinQt base dir not set. Call SetDolphinQtBaseDir() first.";
            return false;
        }
        if (m_imported_from_qt && !force) return true;

        std::string err;
        const fs::path base_user = m_qt_base_dir / "User";
        if (!require_exists_dir(base_user, "User", &err)) { if (error_out) *error_out = err; return false; }
        if (!copy_tree(base_user, m_user_dir, &err)) { if (error_out) *error_out = err; return false; }

        SConfig::GetInstance().LoadSettings();
        if (m_system_pad_is_inited)
        {
            Pad::Shutdown();
            Pad::Initialize();
        }

        m_imported_from_qt = true;
        return true;
    }

    bool DolphinWrapper::ApplyConfig(const savor::SimConfig& cfg, std::string* error_out) {
        if (!SetUserDirectory(cfg.user_dir)) {
            if (error_out) *error_out = "Failed to set user directory: " + cfg.user_dir.string();
            return false;
        }
        std::string err;
        if (!SetDolphinQtBaseDir(cfg.dolphin_base_dir, &err)) {
            if (error_out) *error_out = "Invalid DolphinQt base: " + err;
            return false;
        }
        if (!SyncFromDolphinQtBase(/*force=*/false, &err)) {
            if (error_out) *error_out = "Failed to sync from base: " + err;
            return false;
        }
        return true;
    }

    bool DolphinWrapper::createRenderSurfaceWindow()
    {
        if (m_render_window_handle != nullptr) {
            return true;
        }
#ifdef _WIN32
        static const wchar_t* kClassName = L"SavorCoreVisualViewportWindow";
        static std::once_flag class_once;
        std::call_once(class_once, []() {
            WNDCLASSW wc{};
            wc.lpfnWndProc = SavorCoreViewportWndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = kClassName;
            wc.style = CS_OWNDC;
            RegisterClassW(&wc);
            });

        HWND hwnd = CreateWindowExW(
            0,
            kClassName,
            L"SavorCore Visual Viewport",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            1280, 720,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) {
            return false;
        }
        ShowWindow(hwnd, SW_SHOW);
        m_render_window_handle = hwnd;
        return true;
#else
        return false;
#endif
    }

    void DolphinWrapper::destroyRenderSurfaceWindow()
    {
#ifdef _WIN32
        if (m_render_window_handle != nullptr && m_render_window_handle != m_external_render_widget_handle) {
            DestroyWindow(static_cast<HWND>(m_render_window_handle));
            m_render_window_handle = nullptr;
        }
        if (m_render_window_handle == m_external_render_widget_handle) {
            m_render_window_handle = nullptr;
        }
#else
        m_render_window_handle = nullptr;
#endif
    }

    bool savor::DolphinWrapper::readU8(uint32_t addr, uint8_t& out) const
    {
        if (!isRunning()) return false;

        if (Core::GetState(*m_system) == Core::State::Paused)
        {
            auto& mem = m_system->GetMemory();
            out = mem.Read_U8(addr);
        }
        else {
            Core::CPUThreadGuard guard(Core::System::GetInstance());
            auto& mem = guard.GetSystem().GetMemory();
            out = mem.Read_U8(addr);
        }

        SCLOGT("[mem read] Successfully read u8: 0x%X", out);
        return true;
    }

    bool savor::DolphinWrapper::readU16(uint32_t addr, uint16_t& out) const
    {
        if (!isRunning()) return false;

        if (Core::GetState(*m_system) == Core::State::Paused)
        {
            auto& mem = m_system->GetMemory();
            out = mem.Read_U16(addr);
        }
        else {
            Core::CPUThreadGuard guard(Core::System::GetInstance());
            auto& mem = guard.GetSystem().GetMemory();
            out = mem.Read_U16(addr);
        }

        SCLOGT("[mem read] Successfully read u16: 0x%X", out);
        return true;
    }

    bool savor::DolphinWrapper::readU32(uint32_t addr, uint32_t& out) const
    {
        if (!isRunning()) return false;

        if (Core::GetState(*m_system) == Core::State::Paused)
        {
            auto& mem = m_system->GetMemory();
            out = mem.Read_U32(addr);
        }
        else {
            Core::CPUThreadGuard guard(Core::System::GetInstance());
            auto& mem = guard.GetSystem().GetMemory();
            out = mem.Read_U32(addr);
        }

        SCLOGT("[mem read] Successfully read u32: 0x%X", out);
        return true;
    }

    bool savor::DolphinWrapper::writeU32(uint32_t addr, uint32_t value)
    {
        if (!isRunning()) return false;
        if (Core::GetState(*m_system) != Core::State::Paused)
        {
            SCLOGW("[mem write] Refusing u32 write while core is not paused addr=%08X value=%08X", addr, value);
            return false;
        }

        auto& mem = m_system->GetMemory();
        mem.Write_U32(value, addr);
        SCLOGT("[mem write] Successfully wrote u32: addr=%08X value=%08X", addr, value);
        return true;
    }

    bool savor::DolphinWrapper::readU64(uint32_t addr, uint64_t& out) const
    {
        if (!isRunning()) return false;

        if (Core::GetState(*m_system) == Core::State::Paused)
        {
            auto& mem = m_system->GetMemory();
            out = mem.Read_U64(addr);
        }
        else {
            Core::CPUThreadGuard guard(Core::System::GetInstance());
            auto& mem = guard.GetSystem().GetMemory();
            out = mem.Read_U64(addr);
        }

        SCLOGT("[mem read] Successfully read u32: 0x%X", out);
        return true;
    }

    bool savor::DolphinWrapper::readF32(uint32_t addr, float& out) const
    {
        if (!isRunning()) return false;

        uint32_t u;
        if (Core::GetState(*m_system) == Core::State::Paused)
        {
            auto& mem = m_system->GetMemory();
            u = mem.Read_U32(addr);
        }
        else {
            Core::CPUThreadGuard guard(Core::System::GetInstance());
            auto& mem = guard.GetSystem().GetMemory();
            u = mem.Read_U32(addr);
        }

        out = std::bit_cast<float>(u);
        SCLOGT("[mem read] Successfully read float: %.2f", out);
        return true;
    }

    bool savor::DolphinWrapper::readF64(uint32_t addr, double& out) const
    {
        if (!isRunning()) return false;

        uint64_t u;
        if (Core::GetState(*m_system) == Core::State::Paused)
        {
            auto& mem = m_system->GetMemory();
            u = mem.Read_U64(addr);
        }
        else {
            Core::CPUThreadGuard guard(Core::System::GetInstance());
            auto& mem = guard.GetSystem().GetMemory();
            u = mem.Read_U64(addr);
        }

        out = std::bit_cast<double>(u);
        SCLOGT("[mem read] Successfully read double: %.2f", out);
        return true;
    }

    bool DolphinWrapper::resolveKey(addr::AddrKey k, uint32_t& out_va) const
    {
        out_va = addr::AddrRegistry::base(k);
        return true;
    }

    bool DolphinWrapper::readByKey(addr::AddrKey k, uint8_t& out, bool require_paused) const
    {
        if (!isRunning()) return false;
        if (require_paused && Core::GetState(*m_system) != Core::State::Paused) {
            SCLOGE("[DW] readByKey(%s) denied: core not paused", addr::AddrRegistry::name(k));
            return false;
        }
        uint32_t va = 0;
        if (!resolveKey(k, va)) {
            SCLOGE("[DW] readByKey(%s) resolve failed", addr::AddrRegistry::name(k));
            return false;
        }
        return readU8(va, out);
    }

    bool DolphinWrapper::readByKey(addr::AddrKey k, uint16_t& out, bool require_paused) const
    {
        if (!isRunning()) return false;
        if (require_paused && Core::GetState(*m_system) != Core::State::Paused) {
            SCLOGE("[DW] readByKey(%s) denied: core not paused", addr::AddrRegistry::name(k));
            return false;
        }
        uint32_t va = 0;
        if (!resolveKey(k, va)) {
            SCLOGE("[DW] readByKey(%s) resolve failed", addr::AddrRegistry::name(k));
            return false;
        }
        return readU16(va, out);
    }

    bool DolphinWrapper::readByKey(addr::AddrKey k, uint32_t& out, bool require_paused) const
    {
        if (!isRunning()) return false;
        if (require_paused && Core::GetState(*m_system) != Core::State::Paused) {
            SCLOGE("[DW] readByKey(%s) denied: core not paused", addr::AddrRegistry::name(k));
            return false;
        }
        uint32_t va = 0;
        if (!resolveKey(k, va)) {
            SCLOGE("[DW] readByKey(%s) resolve failed", addr::AddrRegistry::name(k));
            return false;
        }
        return readU32(va, out);
    }

    bool DolphinWrapper::readByKey(addr::AddrKey k, uint64_t& out, bool require_paused) const
    {
        if (!isRunning()) return false;
        if (require_paused && Core::GetState(*m_system) != Core::State::Paused) {
            SCLOGE("[DW] readByKey(%s) denied: core not paused", addr::AddrRegistry::name(k));
            return false;
        }
        uint32_t va = 0;
        if (!resolveKey(k, va)) {
            SCLOGE("[DW] readByKey(%s) resolve failed", addr::AddrRegistry::name(k));
            return false;
        }

        // No existing readU64 raw API in header; use MEM1 snapshot as a fallback:
        std::string mem1;
        if (!getMem1(mem1)) return false;
        MemView view(reinterpret_cast<const uint8_t*>(mem1.data()), mem1.size());
        return view.read_u64(va, out);
    }

    bool DolphinWrapper::readByKeyAny(addr::AddrKey k, uint8_t width, uint64_t& out, uint8_t& out_width) const
    {
        out = 0; out_width = width;
        const auto& spec = addr::AddrRegistry::spec(k);
        switch (width) {
        case 1: { uint8_t  v = 0; if (!readByKey(k, v)) return false; out = v; return true; }
        case 2: { uint16_t v = 0; if (!readByKey(k, v)) return false; out = v; return true; }
        case 4: { uint32_t v = 0; if (!readByKey(k, v)) return false; out = v; return true; }
        case 8: { uint64_t v = 0; if (!readByKey(k, v)) return false; out = v; return true; }
        default: return false;
        }
    }

    bool DolphinWrapper::armPcBreakpoints(const std::vector<uint32_t>& pcs)
    {
        (void)pcs;
        SCLOGE("[core] hard cutover: armPcBreakpoints is disconnected; use StopPointRouter");
        return false;
    }

    bool DolphinWrapper::disarmPcBreakpoints(const std::vector<uint32_t>& pcs)
    {
        (void)pcs;
        SCLOGE("[core] hard cutover: disarmPcBreakpoints is disconnected; use StopPointRouter");
        return false;
    }

    void DolphinWrapper::clearAllPcBreakpoints()
    {
        SCLOGE("[core] hard cutover: clearAllPcBreakpoints is disconnected; use StopPointRouter");
    }

    bool DolphinWrapper::setEnableBreakpoint(uint32_t pc, bool enabled)
    {
        (void)pc;
        (void)enabled;
        SCLOGE("[core] hard cutover: setEnableBreakpoint is disconnected; use StopPointRouter");
        return false;
    }

    bool DolphinWrapper::setEnableAllBreakpoints(bool enabled)
    {
        (void)enabled;
        SCLOGE("[core] hard cutover: setEnableAllBreakpoints is disconnected; use StopPointRouter");
        return false;
    }

    bool DolphinWrapper::setEnabledPcBreakpointsOnly(const std::vector<uint32_t>& enabled_pcs)
    {
        (void)enabled_pcs;
        SCLOGE("[core] hard cutover: setEnabledPcBreakpointsOnly is disconnected; use StopPointRouter");
        return false;
    }

    bool DolphinWrapper::armMemoryWatchpoints(const std::vector<MemoryWatchpointSpec>& specs)
    {
        (void)specs;
        SCLOGE("[core] hard cutover: armMemoryWatchpoints is disconnected; use StopPointRouter");
        return false;
    }

    void DolphinWrapper::clearMemoryWatchpoints()
    {
        SCLOGE("[core] hard cutover: clearMemoryWatchpoints is disconnected; use StopPointRouter");
    }

    void DolphinWrapper::disableThrottle()
    {
        SCLOGE(
            "[DW/run] hard cutover: direct throttle control is disconnected; "
            "use ExecutionEngine");
    }

    void DolphinWrapper::enableThrottle()
    {
        SCLOGE(
            "[DW/run] hard cutover: direct throttle control is disconnected; "
            "use ExecutionEngine");
    }

    bool DolphinWrapper::isMoviePlaying() const
    {
        auto& movie = m_system->GetMovie();
        return movie.IsPlayingInput();
    }

    bool DolphinWrapper::isMoviePlaybackEnded() const
    {
        auto& movie = m_system->GetMovie();
        return !movie.IsPlayingInput();
    }

    uint64_t DolphinWrapper::getCurrentMovieInputCount() const
    {
        auto& movie = m_system->GetMovie();
        return static_cast<uint64_t>(movie.GetCurrentInputCount());
    }

    void DolphinWrapper::silenceStdOutInfo()
    {
        logger::Logger::get().set_stdout_level(logger::Level::Warn);
    }

    void DolphinWrapper::restoreStdOutInfo()
    {
        logger::Logger::get().set_stdout_level(logger::Level::Info);
    }

    bool DolphinWrapper::waitForPausedCoreState(uint32_t timeout_ms, uint32_t poll_rate_ms)
    {
        const auto start = std::chrono::steady_clock::now();
        SCLOGDX(SC_TAGS("run"), "waitForPaused start state=%d timeout=%u", (int)Core::GetState(*m_system), timeout_ms);

        auto deadline = start + std::chrono::milliseconds(timeout_ms);

        while (std::chrono::steady_clock::now() < deadline) {
            if (Core::GetState(*m_system) == Core::State::Paused) 
                break;
            std::this_thread::sleep_until(steady_clock::now() + milliseconds(poll_rate_ms));
        }
        

        bool result = Core::GetState(*m_system) == Core::State::Paused;        
        SCLOGDX(SC_TAGS("run"), "waitForPaused end ok=%d waited_ms=%lld state=%d",
            result ? 1 : 0,
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count(),
            (int)Core::GetState(*m_system));
        return result;
    }

    void DolphinWrapper::sterilizeConfigs()
    {
        if (m_visual_mode) {
            SCLOGT("Setting GFX Backend to D3D11.");
            Config::SetCurrent(Config::MAIN_GFX_BACKEND, std::string("D3D"));
        }
        else {
            SCLOGT("Setting GFX Backend to Null.");
            Config::SetCurrent(Config::MAIN_GFX_BACKEND, std::string("Null"));
        }

        SCLOGT("Turning off background input.");
        Config::SetCurrent(Config::MAIN_INPUT_BACKGROUND_INPUT, false);

        SCLOGT("Installing headless Dolphin alert handler.");
        Config::SetCurrent(Config::MAIN_USE_PANIC_HANDLERS, true);
        Config::SetCurrent(Config::MAIN_ABORT_ON_PANIC_ALERT, false);
        InstallHeadlessDolphinAlertHandler();

        SCLOGT("Enabling Dolphin debugging for breakpoint-driven runs.");
        Config::SetCurrent(Config::MAIN_ENABLE_DEBUGGING, true);

        Config::SetCurrent(Config::MAIN_WIIMOTE_CONTINUOUS_SCANNING, false);
        Config::SetCurrent(Config::MAIN_CONNECT_WIIMOTES_FOR_CONTROLLER_INTERFACE, false);

        SCLOGT("Ensuring memorycard exists");
        setGCMemoryCardA("");
    }

} // namespace savor
