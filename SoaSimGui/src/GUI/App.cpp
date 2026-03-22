#include "App.h"

#include "Widgets/LeftNav.h"
#include "Widgets/StatusBar.h"
#include "Panes/JobsPane.h"
#include "Panes/JobSetsPane.h"
#include "Panes/CoordinatorPane.h"
#include "Panes/PhaseBuilderPane.h"
#include "Panes/BattleRunSettingsPane.h"
#include "Panes/ArtifactsPane.h"
#include "Panes/SeedProbePane.h"
#include "Panes/ExplorerRunsPane.h"
#include "Panes/SettingsPane.h"

#include "../Models/GuiLayoutStore.h"

#include "Utils/IniDoc.h"
#include "Utils/Log.h"
#include "DB/DBCore/DbService.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <fstream>
#include <filesystem>
#include "../Components/FutureQueue.h"
#include "../Components/DropInbox.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static IniDoc g_doc;
static std::string g_layout_blob;

bool GuiApp::createDeviceSwapchain(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL fl_in[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL fl_out{};

    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        fl_in, 2, D3D11_SDK_VERSION, &sd, &swap_, &device_, &fl_out, &ctx_))) return false;

    ID3D11Texture2D* back = nullptr;
    swap_->GetBuffer(0, IID_PPV_ARGS(&back));
    
    if (back){
        device_->CreateRenderTargetView(back, nullptr, &rtv_);
        back->Release();
    }
    return true;
}

void GuiApp::destroyDevice() {
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    if (swap_) { swap_->Release(); swap_ = nullptr; }
    if (ctx_) { ctx_->Release(); ctx_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
}

bool GuiApp::Init(HWND hwnd) {
    hwnd_ = hwnd;
    if (!createDeviceSwapchain(hwnd)) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 16.0f);

    // Merge a symbols font that covers U+23F0..U+25FF (pause, stop, play, etc.)
    ImFontConfig merge{};
    merge.MergeMode = true;
    static const ImWchar shapes_range[] = { 0x23F0, 0x25FF, 0 };
    io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/seguisym.ttf", 16.0f, &merge, shapes_range);


    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device_, ctx_);
    ::DragAcceptFiles(hwnd, true);

    // Load layout (IniDoc in same dir as exe; file: SoaGui.ini)
    {
        std::ifstream f("SoaGui.ini", std::ios::binary);
        if (f) {
            std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            g_doc = IniDoc::parse(text);
            GuiLayoutStore::LoadLayout(g_doc, g_layout_blob);
            if (!g_layout_blob.empty()) ImGui::LoadIniSettingsFromMemory(g_layout_blob.c_str(), (int)g_layout_blob.size());
        }
    }

    // Start DB + heartbeat
    {
        auto db_root_cfg = g_doc.get("Settings", "db_root", "");
        if (!db_root_cfg.empty()) {
            simcore::db::DBService::instance().set_database_root(db_root_cfg);
        }
    }
    simcore::db::DBService::instance().start();
    hb_.start(&status_);
    FutureQueue::Start();

    return true;
}

void GuiApp::Shutdown() {
    hb_.stop();
    StopCoordinator();
    simcore::db::DBService::instance().stop();
    FutureQueue::Stop();

    // Save layout
    {
        size_t sz = 0;
        const char* mem = ImGui::SaveIniSettingsToMemory(&sz);
        g_layout_blob.assign(mem, mem + sz);
        GuiLayoutStore::SaveLayout(g_doc, g_layout_blob);
        std::ofstream f("SoaGui.ini", std::ios::binary | std::ios::trunc);
        f << g_doc.to_string_sorted();
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    destroyDevice();
}

void GuiApp::newFrame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

static GuiPane s_last_pane{};

void GuiApp::RenderFrame() {
    newFrame();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float kTopBarH = 24.0f;                           // matches your existing top offset
    const float kLeftNavW = 250.0f;
    const float status_h = ImGui::GetFrameHeight() + 2.0f + 1.0f;  // taller bar so the green badge doesn't clip

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + kTopBarH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kLeftNavW, vp->Size.y - kTopBarH - status_h), ImGuiCond_Always);
    GuiLeftNav::Draw();

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + kLeftNavW, vp->Pos.y + kTopBarH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x - kLeftNavW, vp->Size.y - kTopBarH - status_h), ImGuiCond_Always);

    bool pane_swap = GuiLeftNav::GetActive() != s_last_pane;
    s_last_pane = GuiLeftNav::GetActive();

    if (pane_swap) SetAcceptExplorerDrops(GuiLeftNav::GetActive() == GuiPane::Artifacts);

    switch (GuiLeftNav::GetActive()) {
    case GuiPane::JobSets:
        if (pane_swap) JobSetsPane::OnActivated();
        JobSetsPane::Draw();
        break;
    case GuiPane::Jobs:
        if (pane_swap) JobsPane::OnActivated();
        JobsPane::Draw();
        break;
    case GuiPane::Workers:
        CoordinatorPane::Draw();
        break;
    case GuiPane::JobBuilder:
        PhaseBuilderPane::Draw();
        break;
    case GuiPane::BattleRunSettings:
        if (pane_swap) brs_pane.OnActivated();
        brs_pane.Draw();
        break; 
    case GuiPane::Artifacts:
        if (pane_swap) ArtifactsPane::OnActivated();
        ArtifactsPane::Draw();
        break;
    case GuiPane::SeedProbe:
        if (pane_swap) SeedProbePane::OnActivated();
        SeedProbePane::Draw();
        break;
    case GuiPane::ExplorerRuns:
        if (pane_swap) ExplorerRunsPane::OnActivated();
        ExplorerRunsPane::Draw();
        break;
    case GuiPane::Settings:
        SettingsPane::Draw();
        break;
    default:
        ImGui::Begin("Content");
        ImGui::TextUnformatted("Coming soon");
        ImGui::End();
        break;
    }


    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - status_h), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, status_h), ImGuiCond_Always);
    GuiStatusBar::Draw(status_);

    ImGui::Render();

    const float clear_col[4] = { 0.10f,0.10f,0.12f,1.0f };
    ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
    ctx_->ClearRenderTargetView(rtv_, clear_col);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swap_->Present(1, 0);
}

void GuiApp::OnResize(UINT w, UINT h) {
    if (!swap_) return;
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* back = nullptr;
    swap_->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back){
        device_->CreateRenderTargetView(back, nullptr, &rtv_);
        back->Release();
    }
}

bool GuiApp::HandleWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DROPFILES) {
        if (!explorer_drops_enabled_) { // shouldn’t happen, but be safe
            DragFinish((HDROP)wParam);
            return true;
        }
        HDROP h = (HDROP)wParam;
        UINT n = DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0);
        POINT pt{};
        DragQueryPoint(h, &pt); // client coords

        std::vector<std::filesystem::path> paths;
        paths.reserve(n);
        for (UINT i = 0; i < n; ++i) {
            wchar_t buf[MAX_PATH]{ 0 };
            DragQueryFileW(h, i, buf, MAX_PATH);
            paths.emplace_back(buf);
        }
        DragFinish(h);

        DropEvent ev{};
        ev.paths = std::move(paths);
        ev.screen_pt = pt;
        DropInbox::Push(std::move(ev));
        return true;
    }
    return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
}

void GuiApp::SetAcceptExplorerDrops(bool enable) {
    if (explorer_drops_enabled_ == enable) return;
    explorer_drops_enabled_ = enable;
    ::DragAcceptFiles(hwnd_, enable ? TRUE : FALSE);
    if (!enable) {
        DropInbox::Clear(); // Defensive: if anything was queued from earlier, drop it
    }
}

bool GuiApp::CoordinatorRunning() const { return wc_ != nullptr; }
void GuiApp::StartCoordinator(WorkerCoordinatorConfig& cfg) {
    if (wc_) return;
    wc_ = std::make_unique<simcore::WorkerCoordinator>(cfg);
    wc_->start();
    paused_ = cfg.start_to_paused;
}
void GuiApp::StopCoordinator() {
    if (!wc_) return;
    wc_->stop();
    wc_.reset();
    desired_workers_ = 0;
    paused_ = false;
}
void GuiApp::SetCoordinatorTargetWorkers(size_t n) {
    desired_workers_ = n > 0 ? n : 1;
    if (wc_) wc_->set_target_workers(desired_workers_);
}
void GuiApp::SetCoordinatorPaused(bool p) {
    paused_ = p;
    if (wc_) wc_->set_paused(paused_);
}
bool   GuiApp::CoordinatorPaused() const { return paused_; }
size_t GuiApp::CoordinatorTargetWorkers() const { return desired_workers_; }
size_t GuiApp::CoordinatorActiveWorkers() const {
    if (!wc_) return 0;
    return wc_->GetClusterSnapshot().size();
}
void   GuiApp::SetCoordinatorEventBufferCapacity(size_t n) {
    if (wc_) wc_->SetEventBufferCapacity(n);
}
std::vector<WorkerSnapshot> GuiApp::CoordinatorSnapshot() const {
    if (!wc_) return {};
    return wc_->GetClusterSnapshot();
}

std::string GuiApp::GuiCfgGet(const std::string& section, const std::string& key, const std::string& def) const {
    // Assumes IniDoc supports get(section,key,default)
    return g_doc.get(section, key, def);
}

void GuiApp::GuiCfgSet(const std::string& section, const std::string& key, const std::string& val) {
    // Assumes IniDoc supports set(section,key,value)
    g_doc.set(section, key, val);
}

int GuiApp::GuiCfgGetInt(const std::string& section, const std::string& key, int def) const {
    std::string s = g_doc.get(section, key, "");
    if (s.empty()) return def;
    try { return std::stoi(s); }
    catch (...) { return def; }
}

void GuiApp::GuiCfgSetInt(const std::string& section, const std::string& key, int v) {
    g_doc.set(section, key, std::to_string(v));
}


std::string GuiApp::GetDatabaseRoot() const {
    return simcore::db::DBService::instance().database_root().string();
}

bool GuiApp::ApplyDatabaseRootChange(const std::string& new_root, std::string& error) {
    const bool had_coordinator = CoordinatorRunning();
    if (had_coordinator) StopCoordinator();

    hb_.stop();
    const bool ok = simcore::db::DBService::instance().relocate_database_root(new_root, true, error);
    hb_.start(&status_);
    return ok;
}
