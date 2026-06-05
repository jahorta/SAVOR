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
#include <cstdarg>
#include <mutex>

#include "Core/PowerPC/BreakPoints.h"
#include <unordered_set>
#include "Shims/StateBufferShim.h"
#include "../Runner/Script/ScriptProgress.h"
#include "../Tas/DtmFile.h"


using namespace std::chrono_literals;
using namespace std::chrono;
namespace fs = std::filesystem;
using addr::AddrKey;
using addr::DolphinAddr;

namespace simcore {

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
    static LRESULT CALLBACK SimCoreViewportWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
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

    static bool initWiimotesNone() {
        using namespace Wiimote;
        // Ensure module is initialized and configs loaded so controllers exist.
        Initialize(InitializeMode::DO_NOT_WAIT_FOR_WIIMOTES);
        if (auto* ic = GetConfig()) {
            ic->LoadConfig();
            SCLOGI("[Wii] controllers=%d", ic->GetControllerCount());
        }
        // Force all Wiimote sources to None, then re-init to materialize devices.
        Config::SetCurrent(Config::MAIN_CONNECT_WIIMOTES_FOR_CONTROLLER_INTERFACE, false);
        Shutdown();
        Initialize(InitializeMode::DO_NOT_WAIT_FOR_WIIMOTES);
        if (auto* ic2 = GetConfig()) {
            ic2->LoadConfig();
            SCLOGI("[Wii] controllers(after None)=%d", ic2->GetControllerCount());
        }
        return true;
    }

    static bool loadDolphinGUISettings(WindowSystemInfo wsi) {
        SCLOGI("Loading Dolphin GUI settings");
        g_controller_interface.Initialize(wsi);
        initPads();
        initWiimotesNone();
        return true;
    }

    // --- Turn off Movie helper --------------------------------------------------

    static void DisarmAnyActiveMovie(Core::System& system)
    {
        using namespace Movie;

        Movie::MovieManager& movie = system.GetMovie();

        if (movie.IsPlayingInput())
            movie.EndPlayInput(false);
        movie.SetReadOnly(true);
    }

    // --- class impl -------------------------------------------------------------

    DolphinWrapper::DolphinWrapper()
    {
        m_system = &Core::System::GetInstance();
        //log::Logger::get().open_file("simcore.log", false);
        //log::Logger::get().set_levels(log::Level::Info, log::Level::Trace);
    }
    

    DolphinWrapper::~DolphinWrapper() {
        shutdownAll();
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

    bool DolphinWrapper::loadGame(const std::string& iso_path)
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

        m_system_pad_is_inited = loadDolphinGUISettings(wsi);

        auto volume = DiscIO::CreateVolume(iso_path);
        if (!volume)
            return false;
        m_disc_info = DiscInfo{ volume->GetGameID(), RegionToString(volume->GetRegion()) };

        auto boot = BootParameters::GenerateFromFile(iso_path);
        if (!BootManager::BootCore(*m_system, std::move(boot), wsi))
            return false;

        m_last_game_iso_path = iso_path;

        auto deadline = std::chrono::steady_clock::now() + 20s;
        while (!Core::IsRunning(*m_system) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_until(steady_clock::now() + milliseconds(1));

        return Core::IsRunning(*m_system);
    }

    bool DolphinWrapper::runOnCpuThread(const std::function<void()>& fn, const bool waitForCompletion) const
    {
        if (!Core::IsRunning(*m_system))
            return false;

        const auto start = std::chrono::steady_clock::now();
        SCLOGD("[DW] runOnCpuThread begin wait=%d running=%d state=%d",
            waitForCompletion ? 1 : 0, Core::IsRunning(*m_system) ? 1 : 0, (int)Core::GetState(*m_system));

        std::atomic<bool> done{ false };
        Core::RunOnCPUThread(*m_system, [&] {
            fn();
            done = true;
            }, waitForCompletion);

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
        if (offset) rtc = rtc - simcore::tas::base_sec;
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
        for (int i = 0; i < section_count - 1; i++) {
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

    bool simcore::DolphinWrapper::getMem1(std::string& out) const
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

        SCLOGD("[DW] loadSavestateFromFile begin path=%s is_cpu_thread=%d state=%d",
            state_path.c_str(), Core::IsCPUThread(), (int)Core::GetState(*m_system));

        uint32_t pc_before = getPC();
        uint64_t tbr_before = getTBR();

        const bool scheduled = runOnCpuThread([&] {
            State::LoadAs(*m_system, state_path);
            }, true);

        // (you already do pc_before/tbr_before)
        SCLOGD("[DW] loadSavestate scheduled=%d", scheduled ? 1 : 0);

        if (!scheduled) {
            return false;
        }

        DisarmAnyActiveMovie(*m_system);

        // Right before the final return
        const uint32_t pc_after = getPC();
        const uint64_t tbr_after = getTBR();

        SCLOGD("[DW] loadSavestate end state=%d pc:%08X->%08X tbr:%016llX->%016llX movie_disarmed",
            (int)Core::GetState(*m_system), pc_before, pc_after,
            (unsigned long long)tbr_before, (unsigned long long)tbr_after);

        if (Core::IsRunning(*m_system) && (pc_before != pc_after || tbr_before != tbr_after || state_path._Equal(m_last_save_state))) {
            m_last_save_state = state_path;
            return true;
        }
        else {
            return false;
        }
    }

    bool DolphinWrapper::saveSavestateBlocking(const std::string& path)
    {
        if (!Core::IsRunning(*m_system)) return false;
        if (Core::GetState(*m_system) != Core::State::Paused) 
            Core::SetState(*m_system, Core::State::Paused, false, false);
        while (Core::GetState(*m_system) != Core::State::Paused)
            std::this_thread::sleep_for(milliseconds(10));
        State::SaveAs(*m_system, path, true);
        return true;
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
        
        SOASim_LoadFromBufferShim(*m_system, buf);

        const uint32_t pc_after = getPC();
        const uint64_t tbr_after = getTBR();
        SCLOGD("[DW] loadStateFromBuffer end   state=%d pc:%08X->%08X tbr:%016llX->%016llX",
            (int)Core::GetState(*m_system), pc_before, pc_after,
            (unsigned long long)tbr_before, (unsigned long long)tbr_after);
        return true;
    }

    bool DolphinWrapper::startMoviePlayback(const std::string& dtm_path)
    {
        SCLOGI("[Movie] PLAY %s", dtm_path);
        if (Core::GetState(*m_system) == Core::State::Paused)
            Core::SetState(*m_system, Core::State::Running);

        while (Core::GetState(*m_system) != Core::State::Running)
            std::this_thread::sleep_until(steady_clock::now() + milliseconds(10));

        Core::Stop(*m_system);
        while (!Core::IsUninitialized(*m_system))
            std::this_thread::sleep_until(steady_clock::now() + milliseconds(10));

        auto& movie = m_system->GetMovie();
        if (!movie.IsReadOnly()) 
            movie.SetReadOnly(true);
        
        if (movie.IsMovieActive())
            movie.EndPlayInput(false);
        
        if (!movie.PlayInput(dtm_path, new std::optional<std::string>{}))
            return false;
        
        auto boot = BootParameters::GenerateFromFile(m_last_game_iso_path);
        if (!BootManager::BootCore(*m_system, std::move(boot), m_wsi))
            return false;

        while (!Core::IsRunning(*m_system))
            std::this_thread::sleep_until(steady_clock::now() + milliseconds(10));
        
        return true;
    }

    bool DolphinWrapper::endMoviePlaybackBlocking(uint32_t timeout_ms)
    {
        
        SCLOGI("[Movie] STOP (request)");
        auto& movie = m_system->GetMovie();
        
        if (Core::GetState(*m_system) == Core::State::Paused) {
            movie.EndPlayInput(false);
            movie.SetReadOnly(false);
        }
        else {
            runOnCpuThread([&] {
                movie.EndPlayInput(false);
                movie.SetReadOnly(false);
                }, true);
        }

        const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
        while (movie.IsPlayingInput() && steady_clock::now() < deadline)
            std::this_thread::sleep_until(steady_clock::now() + milliseconds(10));

        const bool stopped = !movie.IsPlayingInput();
        SCLOGI("[Movie] STOP %s", stopped ? "ok" : "timeout");
        return stopped;
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

                    SCLOGI("[MemCard] Created RAW at %s", memcard_path.string());
                }
            }
            else 
            {
                fs:copy_file(raw_path, memcard_path, fs::copy_options::overwrite_existing);
                SCLOGI("[MemCard] Copied RAW to %s", gc_dir.string());
            }

            SCLOGT("[MemCard] Setting memcard to %s", memcard_path.c_str());
            Config::SetCurrent(Config::MAIN_MEMCARD_A_PATH, memcard_path.string());
            return true;
        }
        catch (...) {
            return false;
        }
    }

    bool DolphinWrapper::startMovieRecording()
    {
        Movie::ControllerTypeArray controllers{ Movie::ControllerType::GC, Movie::ControllerType::None, Movie::ControllerType::None, Movie::ControllerType::None };
        Movie::WiimoteEnabledArray wiimotes{ false, false, false, false };
        
        auto& movie = m_system->GetMovie();
        if (isMoviePlaying())
            movie.EndPlayInput(false);
        
        if (movie.IsReadOnly())
            movie.SetReadOnly(false);
        
        return m_system->GetMovie().BeginRecordingInput(controllers, wiimotes);
    }

    void DolphinWrapper::endMovieRecording(std::optional<std::string> movie_save_path)
    {
        auto& movie = m_system->GetMovie();
        if (movie.IsRecordingInput() && movie_save_path.has_value())
        {
            if (Core::GetState(*m_system) == Core::State::Running)
                Core::SetState(*m_system, Core::State::Paused);
            while (Core::GetState(*m_system) == Core::State::Running)
                std::this_thread::sleep_for(milliseconds(10));
            movie.SaveRecording(movie_save_path.value());
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

    DolphinWrapper::InputTapePlaybackResult DolphinWrapper::playInputTapeBlocking(
        const InputPlan& plan,
        const InputTapePlaybackOptions& options)
    {
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
    }

    // -- Frame Advancing --------------------------------

    bool DolphinWrapper::stepOneFrameBlocking(int timeout_ms)
    {
        if (!Core::IsRunning(*m_system))
            return false;

        SCLOGD("[DW/run] step begin state=%d", (int)Core::GetState(*m_system));
        Core::DoFrameStep(*m_system);   // schedules a single frame and re-pauses

        const bool ok = waitForPausedCoreState(timeout_ms);
        SCLOGD("[DW/run] step end   ok=%d state=%d pc=%08X", ok ? 1 : 0, (int)Core::GetState(*m_system), getPC());
        return ok;
    }

    bool DolphinWrapper::pauseEmulationBlocking(uint32_t timeout_ms)
    {
        if (!m_system || !Core::IsRunning(*m_system))
            return false;
        Core::SetState(*m_system, Core::State::Paused);
        return waitForPausedCoreState(timeout_ms ? timeout_ms : 1000);
    }

    bool DolphinWrapper::resumeEmulation()
    {
        if (!m_system || !Core::IsRunning(*m_system))
            return false;
        Core::SetState(*m_system, Core::State::Running);
        return Core::GetState(*m_system) == Core::State::Running;
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

    bool DolphinWrapper::ApplyConfig(const simcore::SimConfig& cfg, std::string* error_out) {
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
        static const wchar_t* kClassName = L"SimCoreVisualViewportWindow";
        static std::once_flag class_once;
        std::call_once(class_once, []() {
            WNDCLASSW wc{};
            wc.lpfnWndProc = SimCoreViewportWndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = kClassName;
            wc.style = CS_OWNDC;
            RegisterClassW(&wc);
            });

        HWND hwnd = CreateWindowExW(
            0,
            kClassName,
            L"SimCore Visual Viewport",
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

    bool simcore::DolphinWrapper::readU8(uint32_t addr, uint8_t& out) const
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

    bool simcore::DolphinWrapper::readU16(uint32_t addr, uint16_t& out) const
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

    bool simcore::DolphinWrapper::readU32(uint32_t addr, uint32_t& out) const
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

    bool simcore::DolphinWrapper::readU64(uint32_t addr, uint64_t& out) const
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

    bool simcore::DolphinWrapper::readF32(uint32_t addr, float& out) const
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

    bool simcore::DolphinWrapper::readF64(uint32_t addr, double& out) const
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
        out_va = addr::Registry::base(k);
        return true;
    }

    bool DolphinWrapper::readByKey(addr::AddrKey k, uint8_t& out, bool require_paused) const
    {
        if (!isRunning()) return false;
        if (require_paused && Core::GetState(*m_system) != Core::State::Paused) {
            SCLOGE("[DW] readByKey(%s) denied: core not paused", addr::Registry::name(k));
            return false;
        }
        uint32_t va = 0;
        if (!resolveKey(k, va)) {
            SCLOGE("[DW] readByKey(%s) resolve failed", addr::Registry::name(k));
            return false;
        }
        return readU8(va, out);
    }

    bool DolphinWrapper::readByKey(addr::AddrKey k, uint16_t& out, bool require_paused) const
    {
        if (!isRunning()) return false;
        if (require_paused && Core::GetState(*m_system) != Core::State::Paused) {
            SCLOGE("[DW] readByKey(%s) denied: core not paused", addr::Registry::name(k));
            return false;
        }
        uint32_t va = 0;
        if (!resolveKey(k, va)) {
            SCLOGE("[DW] readByKey(%s) resolve failed", addr::Registry::name(k));
            return false;
        }
        return readU16(va, out);
    }

    bool DolphinWrapper::readByKey(addr::AddrKey k, uint32_t& out, bool require_paused) const
    {
        if (!isRunning()) return false;
        if (require_paused && Core::GetState(*m_system) != Core::State::Paused) {
            SCLOGE("[DW] readByKey(%s) denied: core not paused", addr::Registry::name(k));
            return false;
        }
        uint32_t va = 0;
        if (!resolveKey(k, va)) {
            SCLOGE("[DW] readByKey(%s) resolve failed", addr::Registry::name(k));
            return false;
        }
        return readU32(va, out);
    }

    bool DolphinWrapper::readByKey(addr::AddrKey k, uint64_t& out, bool require_paused) const
    {
        if (!isRunning()) return false;
        if (require_paused && Core::GetState(*m_system) != Core::State::Paused) {
            SCLOGE("[DW] readByKey(%s) denied: core not paused", addr::Registry::name(k));
            return false;
        }
        uint32_t va = 0;
        if (!resolveKey(k, va)) {
            SCLOGE("[DW] readByKey(%s) resolve failed", addr::Registry::name(k));
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
        const auto& spec = addr::Registry::spec(k);
        switch (width) {
        case 1: { uint8_t  v = 0; if (!readByKey(k, v)) return false; out = v; return true; }
        case 2: { uint16_t v = 0; if (!readByKey(k, v)) return false; out = v; return true; }
        case 4: { uint32_t v = 0; if (!readByKey(k, v)) return false; out = v; return true; }
        case 8: { uint64_t v = 0; if (!readByKey(k, v)) return false; out = v; return true; }
        default: return false;
        }
    }

    static bool contains_pc(const std::unordered_set<uint32_t>& s, uint32_t v) { return s.find(v) != s.end(); }
    struct ArmedSet { std::unordered_set<uint32_t> pcs; };
    static ArmedSet& armed_singleton() { static ArmedSet a; return a; }
    static ArmedSet& armed_battle_singleton() { static ArmedSet a; return a; }

    bool DolphinWrapper::armBattleBreakpoints()
    {
        if (!armed_battle_singleton().pcs.empty()) return true;

        auto pcs = simcore::progress::BattleProgressBPs();

        SCLOGT("[core] arming battle breakpoints");
        auto& armed = armed_battle_singleton().pcs;
        bool arm_result = runOnCpuThread([&] {
            for (auto pc : pcs)
            {
                if (armed.insert(pc).second)
                    m_system->GetPowerPC().GetBreakPoints().Add(pc);
            }
            }, true);
        SCLOGT("[core] properly loaded battle breakpoints: %s", arm_result ? "true" : "false");
        SCLOGT("[core] checking current battle breakpoints");
        for (auto bp : m_system->GetPowerPC().GetBreakPoints().GetStrings()) {
            SCLOGT("[core] Battle Breakpoint Present: %s", bp.c_str());
        }
        return arm_result;
    }

    bool DolphinWrapper::disarmBattleBreakpoints()
    {
        if (armed_battle_singleton().pcs.empty()) return true;

        auto pcs = simcore::progress::BattleProgressBPs();

        SCLOGT("[core] disarming battle breakpoints");
        auto& armed = armed_battle_singleton().pcs;
        bool disarm_result = runOnCpuThread([&] {
            for (auto pc : pcs)
            {
                auto it = armed.find(pc);
                if (it != armed.end())
                {
                    m_system->GetPowerPC().GetBreakPoints().Remove(pc);
                    armed.erase(it);
                }
            }
            }, true);

        SCLOGT("[core] properly removed battle breakpoints: %s", disarm_result ? "true" : "false");
        SCLOGT("[core] checking current battle breakpoints");
        for (auto bp : m_system->GetPowerPC().GetBreakPoints().GetStrings()) {
            SCLOGT("[core] Battle Breakpoint Present: %s", bp.c_str());
        }
        return disarm_result;
    }

    bool DolphinWrapper::armPcBreakpoints(const std::vector<uint32_t>& pcs)
    {
        SCLOGT("[core] arming breakpoints");
        auto& armed = armed_singleton().pcs;
        bool arm_result = runOnCpuThread([&] {
            for (auto pc : pcs)
            {
                if (armed.insert(pc).second)
                    m_system->GetPowerPC().GetBreakPoints().Add(pc);
            }
            }, true);
        SCLOGT("[core] properly loaded breakpoints: %s", arm_result ? "true" : "false");
        SCLOGT("[core] checking current breakpoints");
        for (auto bp : m_system->GetPowerPC().GetBreakPoints().GetStrings()) {
            SCLOGT("[core] Breakpoint Present: %s", bp.c_str());
        }
        return arm_result;
    }

    bool DolphinWrapper::disarmPcBreakpoints(const std::vector<uint32_t>& pcs)
    {
        SCLOGT("[core] disarming breakpoints");
        auto& armed = armed_singleton().pcs;
        bool disarm_result = runOnCpuThread([&] {
            for (auto pc : pcs)
            {
                auto it = armed.find(pc);
                if (it != armed.end())
                {
                    m_system->GetPowerPC().GetBreakPoints().Remove(pc);
                    armed.erase(it);
                }
            }
            }, true);

        SCLOGT("[core] properly removed breakpoints: %s", disarm_result ? "true" : "false");
        SCLOGT("[core] checking current breakpoints");
        for (auto bp : m_system->GetPowerPC().GetBreakPoints().GetStrings()) {
            SCLOGT("[core] Breakpoint Present: %s", bp.c_str());
        }
        return disarm_result;
    }

    void DolphinWrapper::clearAllPcBreakpoints()
    {
        SCLOGT("[core] disarming all breakpoints");
        auto& armed = armed_singleton().pcs;
        bool disarm_result = runOnCpuThread([&] {
            for (auto pc : armed) m_system->GetPowerPC().GetBreakPoints().Remove(pc);
            armed.clear();
            }, true);
        SCLOGT("[core] properly removed breakpoints: %s", disarm_result ? "true" : "false");
    }

    bool DolphinWrapper::setEnableBreakpoint(uint32_t pc, bool enabled)
    {
        SCLOGT("[core] silencing all breakpoints");
        bool silence_result = runOnCpuThread([&] {
            if (m_system->GetPowerPC().GetBreakPoints().IsBreakPointEnable(pc) != enabled)
                m_system->GetPowerPC().GetBreakPoints().ToggleEnable(pc);
            }, true);

        return false;
    }

    bool DolphinWrapper::setEnableAllBreakpoints(bool enabled)
    {
        SCLOGT("[core] silencing all breakpoints");
        auto& armed = armed_singleton().pcs; bool silence_result = runOnCpuThread([&] {
            for (auto pc : armed)
            {
                if (m_system->GetPowerPC().GetBreakPoints().IsBreakPointEnable(pc) != enabled) 
                    m_system->GetPowerPC().GetBreakPoints().ToggleEnable(pc);
            }
            }, true);
        
        return false;
    }

    DolphinWrapper::RunUntilHitResult DolphinWrapper::runUntilBreakpointBlocking(uint32_t timeout_ms)
    {
        // Preserve legacy behavior but now through the flexible watchdog loop with no extra checks.
        return runUntilBreakpointFlexible(timeout_ms, 0, false);
    }

    

    uint32_t DolphinWrapper::pickPollIntervalMs(uint32_t timeout_ms)
    {
        return pickPollIntervalMsForTimeLeft(timeout_ms, timeout_ms);
    }

    uint32_t DolphinWrapper::pickPollIntervalMsForTimeLeft(uint32_t timeout_ms, uint32_t time_left_ms)
    {
        // Monotonic tiers: tighten as we get closer to the deadline.
        // You can tweak these in one place and both VM and wrapper will follow.
        (void)timeout_ms; // reserved for future policy that also considers absolute scale
        if (time_left_ms >= 5u * 60u * 1000u) return 1000u;  // >= 5 minutes
        if (time_left_ms >= 60u * 1000u)      return 500u;  // 1-5 minutes
        if (time_left_ms >= 10u * 1000u)      return 200u;  // 10-60 seconds
        if (time_left_ms >= 2000u)            return 100u;   // 2-10 seconds
        return 100u;                                         // < 2 seconds
    }

    DolphinWrapper::RunUntilHitResult
        DolphinWrapper::runUntilBreakpointFlexible(uint32_t timeout_ms,
            uint32_t vi_stall_ms,
            bool watch_movie,
            uint32_t poll_ms,
            uint32_t progflags,
            ProgressSink sink)
    {
        using std::chrono::steady_clock;
        using std::chrono::milliseconds;

        const auto start = steady_clock::now();
        const auto deadline_initial = start + milliseconds(timeout_ms);

        auto deadline = deadline_initial;
        auto& movie = m_system->GetMovie();
        const bool had_movie = watch_movie && movie.IsPlayingInput();

        
        SCLOGD("[run] timeout set to: %lld ms", timeout_ms);
        SCLOGD("[run] start=%s deadline=%s", simcore::time_util::steady_to_cstr(start), simcore::time_util::steady_to_cstr(deadline));

        // VI stall tracking baseline
        resetViCounterBaseline();
        uint64_t last_vi = getViFieldCountApproxFromBaseline();
        auto last_vi_change = steady_clock::now();

        const ProgressSink& emit = sink ? sink : m_progress_sink; // toggle: null = no progress
        auto last_emit = steady_clock::time_point{};

        const uint32_t pc = getPC();
        if (contains_pc(armed_singleton().pcs, pc)) {
            SCLOGD("[DW/run] Stepping past pc=%08X to avoid breakpoint", pc);
            //setEnableBreakpoint(pc, false);
            Common::Event sync_event;
            auto& power_pc = m_system->GetPowerPC();
            PowerPC::CoreMode old_mode = power_pc.GetMode();
            power_pc.SetMode(PowerPC::CoreMode::Interpreter);
            m_system->GetCPU().StepOpcode(&sync_event);
            sync_event.WaitFor(std::chrono::milliseconds(20));
            power_pc.SetMode(old_mode);
            //setEnableBreakpoint(pc, true);
        }

        if (progflags & (uint32_t)CoreProgressFlags::BattleProgress) armBattleBreakpoints();
        else disarmBattleBreakpoints();

        // Ensure we begin in Running so time can advance (unless already paused by a BP before entry)
        if (Core::GetState(*m_system) != Core::State::Paused)
            Core::SetState(*m_system, Core::State::Running);

        // m_system->GetSystemTimers().GetLocalTimeRTCOffset()
        uint32_t rtc = Config::Get(Config::MAIN_CUSTOM_RTC_VALUE);

        size_t polls = 0;
        while (true)
        {
            const auto now = steady_clock::now();
            if (now >= deadline) {
                // TIMEOUT: enforce postcondition (Paused) then return
                Core::SetState(*m_system, Core::State::Paused);
                SCLOGD("[DW/run] TIMEOUT polls=%zu pc=%08X vi=%lld", polls, getPC(), getViFieldCountApproxFromBaseline());
                return { false, 0u, "timeout" };
            }

            const auto st = Core::GetState(*m_system);
            if (st == Core::State::Paused)
            {
                // 1) Breakpoint takes precedence when paused
                const uint32_t pc = getPC();
                if (contains_pc(armed_singleton().pcs, pc)) {
                    // HIT: core is already Paused by the BP; leave paused and return
                    SCLOGD("[DW/run] HIT pc=%08X polls=%zu", pc, polls);
                    return { true, pc, "breakpoint" };
                }

                // 2) If movie EOM caused the pause (pause-on-EOM enabled), detect and return without resuming
                if (had_movie && !movie.IsPlayingInput()) {
                    Core::SetState(*m_system, Core::State::Paused); // ensure postcondition
                    SCLOGD("[DW/run] MOVIE_ENDED (paused) polls=%zu pc=%08X", polls, getPC());
                    return { false, 0u, "movie_ended" };
                }
            }
            else // Running
            {
                // EOM detection while running
                if (had_movie && !movie.IsPlayingInput()) {
                    Core::SetState(*m_system, Core::State::Paused); // ensure postcondition
                    SCLOGD("[DW/run] MOVIE_ENDED polls=%zu pc=%08X", polls, getPC());
                    return { false, 0u, "movie_ended" };
                }

                // VI-stall detection
                if (vi_stall_ms > 0) {
                    const uint64_t vi_now = getViFieldCountApproxFromBaseline();
                    if (vi_now != last_vi) {
                        last_vi = vi_now;
                        last_vi_change = now;
                    }
                    else {
                        const auto since_ms = std::chrono::duration_cast<milliseconds>(now - last_vi_change).count();
                        if (since_ms >= vi_stall_ms) {
                            Core::SetState(*m_system, Core::State::Paused); // ensure postcondition
                            SCLOGD("[DW/run] VI_STALLED polls=%zu pc=%08X", polls, getPC());
                            return { false, 0u, "vi_stalled" };
                        }
                    }
                }
            }

            if (emit)
            {
                const auto now2 = steady_clock::now();
                const bool time_ok = (!last_emit.time_since_epoch().count()) ||
                    (now2 - last_emit) >= milliseconds(std::max<uint32_t>(100u, poll_ms ? poll_ms : 0u));

                std::vector<std::string> msg_strs{};
                uint32_t flags = 0;

                if (time_ok || ((polls & 0x3F) == 0)) // also piggyback on your 64-poll trace cadence
                {
                    flags |= PF_WAITING_FOR_BP;
                    if (watch_movie && movie.IsPlayingInput()) flags |= PF_MOVIE_PLAYING;

                    // Warn if near timeout
                    const auto left_ms_now2 = (uint32_t)std::chrono::duration_cast<milliseconds>(deadline - now2).count();
                    if (left_ms_now2 <= std::max<uint32_t>(2000u, timeout_ms / 10u))
                    {
                        SCLOGW("[run] close to timeout! time remaining: %lld ms", left_ms_now2);
                        flags |= PF_TIMEOUT_NEAR;
                    }

                    // VI stall early warning (if configured)
                    if (vi_stall_ms > 0)
                    {
                        const uint64_t vi_now = getViFieldCountApproxFromBaseline();
                        if (vi_now == last_vi)
                        {
                            const auto since_ms = (uint32_t)std::chrono::duration_cast<milliseconds>(now2 - last_vi_change).count();
                            if (since_ms >= (vi_stall_ms / 2u))
                            {
                                SCLOGW("[run] close to VI stall!", left_ms_now2);
                                if (progflags & (uint32_t)CoreProgressFlags::WarnViStall) msg_strs.push_back("VI stall imminent");
                            }
                        }
                    }

                    const uint32_t cur_frames = (uint32_t)getFrameCountApprox(false);

                    if (progflags & (uint32_t)CoreProgressFlags::ViDelta) 
                    {
                        SCLOGTX(SC_TAGS("progress"), std::format("VIDelta={}", cur_frames).c_str());
                        msg_strs.push_back(std::format("VIDelta={}", cur_frames));
                    }
                    if (progflags & (uint32_t)CoreProgressFlags::Filename) 
                    {
                        std::string msg = getCurrentSctFileTag();
                        SCLOGTX(SC_TAGS("progress"), msg.c_str());
                        msg_strs.push_back(msg);
                    }
                    if (progflags & (uint32_t)CoreProgressFlags::ScriptSection) 
                    {
                        std::string msg = getCurrentSctSection();
                        SCLOGTX(SC_TAGS("progress"), msg.c_str());
                        msg_strs.push_back(getCurrentSctSection());
                    }

                    last_emit = now2;
                }

                if (progflags & (uint32_t)CoreProgressFlags::BattleProgress && Core::GetState(*m_system) == Core::State::Paused) {
                    uint32_t cur_pc = getPC();
                    
                    if (simcore::progress::BattleProgressBPs().contains(cur_pc))
                    {
                        for (simcore::progress::BattleProgressEntry entry : simcore::progress::BattleProgress) {
                            if (entry.key == cur_pc) 
                            {
                                std::string msg = entry.fxn(*this);
                                SCLOGTX(SC_TAGS("progress"), msg.c_str());
                                msg_strs.push_back(msg);
                            }
                        }
                        SCLOGDX(SC_TAGS("run"), "Stepping past pc=%08X to avoid battle breakpoint", cur_pc);
                        Common::Event sync_event;
                        auto& power_pc = m_system->GetPowerPC();
                        PowerPC::CoreMode old_mode = power_pc.GetMode();
                        power_pc.SetMode(PowerPC::CoreMode::Interpreter);
                        m_system->GetCPU().StepOpcode(&sync_event);
                        sync_event.WaitFor(std::chrono::milliseconds(20));
                        power_pc.SetMode(old_mode);
                    }

                }

                bool record = true;
                if (msg_strs.empty() && time_ok && (progflags & (uint32_t)CoreProgressFlags::DontRecordHeartbeat) != 0) record = false;

                if (msg_strs.empty() && time_ok) msg_strs.push_back((flags & PF_MOVIE_PLAYING) ? "playing" : "waiting on bp");
                
                if (!msg_strs.empty())
                {
                    std::string message{};
                    bool first = true;
                    for (auto s : msg_strs) 
                    {
                        if (s.empty()) continue;
                        if (first) first = false;
                        else message += " - ";
                        message += s;
                    }
                    emit(message.c_str(), record);
                }
            }

            if ((polls++ & 0x3F) == 0) {
                SCLOGDX(SC_TAGS("run"), "poll=%zu state=%d pc=%08X movie=%d vi=%llu",
                    polls, (int)Core::GetState(*m_system), getPC(),
                    movie.IsPlayingInput() ? 1 : 0,
                    (unsigned long long)getViFieldCountApproxFromBaseline());
            }

            // Dynamic poll interval based on *time remaining* (single-sourced policy)
            uint32_t dyn_poll = poll_ms;
            if (dyn_poll == 0) {
                const auto left_ms = (uint32_t)std::chrono::duration_cast<milliseconds>(deadline - now).count();
                dyn_poll = DolphinWrapper::pickPollIntervalMsForTimeLeft(timeout_ms, left_ms);
            }

            // Stall guard: keep polling frequent enough to detect within the configured stall window
            if (vi_stall_ms > 0) {
                const uint32_t guard = std::max<uint32_t>(1u, vi_stall_ms / 2u);
                if (dyn_poll > guard) dyn_poll = guard;
            }

            // Do not oversleep past the deadline
            const auto left_ms_now = (uint32_t)std::chrono::duration_cast<milliseconds>(deadline - steady_clock::now()).count();
            const uint32_t sleep_ms = (left_ms_now > dyn_poll) ? dyn_poll : std::max<uint32_t>(1u, left_ms_now);

            if (Core::GetState(*m_system) == Core::State::Paused)
                Core::SetState(*m_system, Core::State::Running);

            std::this_thread::sleep_for(milliseconds(sleep_ms));
        }
    }

    void DolphinWrapper::disableThrottle()
    {
        Core::SetIsThrottlerTempDisabled(true);
    }

    void DolphinWrapper::enableThrottle()
    {
        Core::SetIsThrottlerTempDisabled(false);
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

        //SCLOGT("Turning off alerts.");
        //Config::SetCurrent(Config::MAIN_);
        Config::SetCurrent(Config::MAIN_WIIMOTE_CONTINUOUS_SCANNING, false);
        Config::SetCurrent(Config::MAIN_CONNECT_WIIMOTES_FOR_CONTROLLER_INTERFACE, false);

        SCLOGT("Ensuring memorycard exists");
        setGCMemoryCardA("");
    }

} // namespace simcore
