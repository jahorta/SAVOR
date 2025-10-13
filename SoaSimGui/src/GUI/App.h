#pragma once
#include <windows.h>
#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#include "imgui.h"
#include "../Models/GuiStatus.h"
#include "../Components/GuiHeartbeatPoller.h"

#include "Runner/Parallel/DB/DBWorkerCoordinatorConfig.h"
#include "Runner/Parallel/DB/DBWorkerCoordinator.h"
#include "Panes/BattleRunSettingsPane.h"

class GuiApp {
public:
    bool Init(HWND hwnd);
    void Shutdown();
    void RenderFrame();
    void OnResize(UINT w, UINT h);
    bool HandleWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    
    // --- Coordinator control (App-owned) ---
    bool CoordinatorRunning() const;
    void StartCoordinator(WorkerCoordinatorConfig& cfg);
    void StopCoordinator();
    void SetCoordinatorTargetWorkers(size_t n);
    void SetCoordinatorPaused(bool p);
    bool CoordinatorPaused() const;
    size_t CoordinatorTargetWorkers() const;
    size_t CoordinatorActiveWorkers() const;
    void   SetCoordinatorEventBufferCapacity(size_t n);
    std::vector<WorkerSnapshot> CoordinatorSnapshot() const;

    std::string GuiCfgGet(const std::string& section, const std::string& key, const std::string& def = "") const;
    void        GuiCfgSet(const std::string& section, const std::string& key, const std::string& val);

    int         GuiCfgGetInt(const std::string& section, const std::string& key, int def = 0) const;
    void        GuiCfgSetInt(const std::string& section, const std::string& key, int v);

private:
    bool createDeviceSwapchain(HWND hwnd);
    void destroyDevice();
    void newFrame();

    HWND hwnd_{};
    ID3D11Device* device_{};
    ID3D11DeviceContext* ctx_{};
    IDXGISwapChain* swap_{};
    ID3D11RenderTargetView* rtv_{};

    GuiStatusModel status_;
    GuiHeartbeatPoller hb_;

    soasim::ui::BattleRunSettingsPane brs_pane{};

    std::unique_ptr<simcore::WorkerCoordinator> wc_;
    size_t desired_workers_{ 0 };
    bool paused_{ false };
};
