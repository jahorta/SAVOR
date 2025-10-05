#include "App.h"

#include "Widgets/LeftNav.h"
#include "Widgets/StatusBar.h"
#include "Panes/JobsPane.h"

#include "../Models/GuiLayoutStore.h"

#include "Utils/IniDoc.h"
#include "Utils/Log.h"
#include "DB/DBCore/DbService.h"

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include <d3d11.h>
#include <fstream>

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
    device_->CreateRenderTargetView(back, nullptr, &rtv_);
    if (back) back->Release();
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
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device_, ctx_);

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
    simcore::db::DBService::instance().start();
    hb_.start(&status_);

    return true;
}

void GuiApp::Shutdown() {
    hb_.stop();

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

    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

    // First-run layout hint (only if no layout loaded)
    if (g_layout_blob.empty()) {
        ImGui::SetNextWindowDockID(ImGui::GetMainViewport()->ID, ImGuiCond_FirstUseEver);
    }
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
    ImGui::SetNextWindowViewport(vp->ID);
    GuiLeftNav::Draw();

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + kLeftNavW, vp->Pos.y + kTopBarH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x - kLeftNavW, vp->Size.y - kTopBarH - status_h), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(vp->ID);
    bool pane_swap = GuiLeftNav::GetActive() != s_last_pane;
    s_last_pane = GuiLeftNav::GetActive();
    switch (GuiLeftNav::GetActive()) {
    case GuiPane::Jobs:
        if (pane_swap) JobsPane::OnActivated();
        JobsPane::Draw();
        break;
    default:
        ImGui::Begin("Content", nullptr, ImGuiWindowFlags_NoMove);
        ImGui::TextUnformatted("Coming soon");
        ImGui::End();
        break;
    }

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - status_h), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, status_h), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(vp->ID);
    GuiStatusBar::Draw(status_);

    ImGui::Render();

    const float clear_col[4] = { 0.10f,0.10f,0.12f,1.0f };
    ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
    ctx_->ClearRenderTargetView(rtv_, clear_col);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swap_->Present(1, 0);

    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

void GuiApp::OnResize(UINT w, UINT h) {
    if (!swap_) return;
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* back = nullptr;
    swap_->GetBuffer(0, IID_PPV_ARGS(&back));
    device_->CreateRenderTargetView(back, nullptr, &rtv_);
    if (back) back->Release();
}

bool GuiApp::HandleWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
}
