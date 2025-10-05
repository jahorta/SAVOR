#pragma once
#include <windows.h>
#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#include "imgui.h"
#include "../Models/GuiStatus.h"
#include "../Components/GuiHeartbeatPoller.h"

class GuiApp {
public:
    bool Init(HWND hwnd);
    void Shutdown();
    void RenderFrame();
    void OnResize(UINT w, UINT h);
    bool HandleWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

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
};
