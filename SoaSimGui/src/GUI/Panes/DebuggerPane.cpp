#include "DebuggerPane.h"

#include "../App.h"
#include "../Widgets/LeftNav.h"
#include "../Components/ToastBus.h"
#include "Runner/Debug/VideoFrameRing.h"
#include "imgui.h"
#include <string>
#include <cstring>
#include <vector>
#include <algorithm>
#include <chrono>
#include <d3d11.h>

namespace {
    int64_t g_selected_session_id = 0;

    struct DebugVideoRenderer {
        simcore::debug::VideoFrameRingConsumer consumer;
        std::string active_ring;
        ID3D11Texture2D* tex{ nullptr };
        ID3D11ShaderResourceView* srv{ nullptr };
        uint32_t tex_w{ 0 };
        uint32_t tex_h{ 0 };
        uint64_t last_frame_id{ 0 };
        std::chrono::steady_clock::time_point next_poll{};

        ~DebugVideoRenderer() { Reset(); }

        void Reset() {
            consumer.Close();
            active_ring.clear();
            last_frame_id = 0;
            tex_w = tex_h = 0;
            if (srv) { srv->Release(); srv = nullptr; }
            if (tex) { tex->Release(); tex = nullptr; }
        }

        bool EnsureOpened(const std::string& ring) {
            if (ring.empty()) return false;
            if (ring == active_ring && consumer.IsOpen()) return true;
            Reset();
            if (!consumer.Open(ring)) return false;
            active_ring = ring;
            return true;
        }

        void EnsureTexture(ID3D11Device* dev, uint32_t w, uint32_t h) {
            if (!dev) return;
            if (tex && tex_w == w && tex_h == h) return;
            if (srv) { srv->Release(); srv = nullptr; }
            if (tex) { tex->Release(); tex = nullptr; }

            D3D11_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) return;

            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = td.Format;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MostDetailedMip = 0;
            sd.Texture2D.MipLevels = 1;
            if (FAILED(dev->CreateShaderResourceView(tex, &sd, &srv))) {
                tex->Release();
                tex = nullptr;
                return;
            }
            tex_w = w;
            tex_h = h;
        }

        void PollAndUpload(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
            if (!consumer.IsOpen() || !dev || !ctx) return;
            const auto now = std::chrono::steady_clock::now();
            if (now < next_poll) return;
            next_poll = now + std::chrono::milliseconds(16);

            simcore::debug::VideoFrameDesc fd{};
            std::vector<uint8_t> pixels;
            if (!consumer.ReadLatest(fd, pixels)) return;
            if (fd.frame_id == last_frame_id) return;
            if (fd.width == 0 || fd.height == 0 || fd.stride == 0 || pixels.empty()) return;

            EnsureTexture(dev, fd.width, fd.height);
            if (!tex) return;

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;

            const uint8_t* src = pixels.data();
            uint8_t* dst = reinterpret_cast<uint8_t*>(mapped.pData);
            const uint32_t rows = fd.height;
            const uint32_t row_bytes = (fd.stride > 0) ? fd.stride : (fd.width * 4u);
            for (uint32_t y = 0; y < rows; ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * mapped.RowPitch, src + static_cast<size_t>(y) * row_bytes, (std::min)(row_bytes, mapped.RowPitch));
            }
            ctx->Unmap(tex, 0);
            last_frame_id = fd.frame_id;
        }

        void DrawImGui(float avail_w, float max_h = 360.0f) {
            if (!srv || tex_w == 0 || tex_h == 0) {
                ImGui::TextUnformatted("No video frame available yet.");
                return;
            }
            const float aspect = static_cast<float>(tex_w) / static_cast<float>(tex_h);
            float draw_w = avail_w;
            float draw_h = draw_w / aspect;
            if (draw_h > max_h) {
                draw_h = max_h;
                draw_w = draw_h * aspect;
            }
            ImGui::Image(reinterpret_cast<ImTextureID>(srv), ImVec2(draw_w, draw_h));
        }
    };

    DebugVideoRenderer g_video;
}

void DebuggerPane::OnActivated() {}

void DebuggerPane::Draw() {
    ImGui::Begin("Debugger", nullptr, ImGuiWindowFlags_NoMove);

    auto lr = g_app.ListRecentVisualDebugSessions(100);
    if (!lr.ok) {
        ImGui::Text("Failed to query debug sessions: %s", lr.error.message.c_str());
        ImGui::End();
        return;
    }

    bool has_active = false;
    for (const auto& s : lr.value) {
        if (simcore::db::DebugSessionsRepo::IsActiveState(s.state)) {
            has_active = true;
            break;
        }
    }
    GuiLeftNav::SetDebuggerHookActive(has_active);

    if (ImGui::BeginTable("dbg_sessions", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Session");
        ImGui::TableSetupColumn("Job");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Started By");
        ImGui::TableHeadersRow();
        for (const auto& s : lr.value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool sel = g_selected_session_id == s.id;
            if (ImGui::Selectable(std::to_string((long long)s.id).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) g_selected_session_id = s.id;
            ImGui::TableSetColumnIndex(1); ImGui::Text("%lld", (long long)s.job_id);
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(s.state.c_str());
            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(s.started_by.value_or("<unknown>").c_str());
        }
        ImGui::EndTable();
    }

    if (g_selected_session_id > 0) {
        ImGui::Separator();
        auto dr = g_app.GetVisualDebugRuntimeSnapshot(g_selected_session_id);
        if (dr.ok) {
            const auto& rt = dr.value;
            ImGui::Text("Session %lld / Job %lld", (long long)rt.session_id, (long long)rt.job_id);
            ImGui::Text("VM: %s | EMU: %s | MODE: %s", rt.vm_state.c_str(), rt.emu_state.c_str(), rt.ux_mode.c_str());
            ImGui::Text("Script: %s @ 0x%08X", rt.script_name.c_str(), rt.script_pc);
            ImGui::Text("Input: %s", rt.current_input.c_str());
            ImGui::Text("Frame: %lld | Seq: %lld", (long long)rt.frame_index, (long long)rt.sequence);
            ImGui::Text("Video: %ux%u %s %s", rt.video_width, rt.video_height, rt.video_pixel_format.c_str(), rt.video_color_space.c_str());

            if (ImGui::Button("Step VM")) (void)g_app.StepVisualDebugVmInstruction(g_selected_session_id);
            ImGui::SameLine();
            if (ImGui::Button("Step Frame")) (void)g_app.StepVisualDebugFrame(g_selected_session_id);
            ImGui::SameLine();
            if (ImGui::Button("Run To BP")) (void)g_app.RunVisualDebugToBreakpoint(g_selected_session_id);
            ImGui::SameLine();
            if (ImGui::Button("Pause")) (void)g_app.PauseVisualDebug(g_selected_session_id);
            ImGui::SameLine();
            if (ImGui::Button("Stop")) {
                auto rr = g_app.StopVisualDebug(g_selected_session_id);
                if (!rr.ok) GuiToastBus::Error("Stop Debugging failed", rr.error.message);
                else GuiToastBus::Warn("Debug session stopped");
                g_video.Reset();
            }

            ImGui::SeparatorText("Script Steps (VM instruction indices)");
            ImGui::TextUnformatted("These rows are VM instruction indices used by Step VM and breakpoint toggles.");
            if (ImGui::BeginTable("dbg_script_steps", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn(" ", ImGuiTableColumnFlags_WidthFixed, 28.0f);
                ImGui::TableSetupColumn("Script step / instruction");
                for (const auto& step : rt.script_steps) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    const bool enabled = std::find(rt.breakpoints.begin(), rt.breakpoints.end(), step.step_id) != rt.breakpoints.end();
                    ImGui::PushID(static_cast<int>(step.step_id));
                    if (enabled) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 70, 70, 255));
                    if (ImGui::SmallButton(enabled ? "●" : " ")) {
                        auto tr = g_app.ToggleVisualDebugBreakpoint(g_selected_session_id, step.step_id, !enabled);
                        if (!tr.ok) GuiToastBus::Error("Toggle breakpoint failed", tr.error.message);
                    }
                    if (enabled) ImGui::PopStyleColor();
                    ImGui::PopID();

                    ImGui::TableSetColumnIndex(1);
                    const uint32_t step_pc = static_cast<uint32_t>(step.step_id * 4);
                    const bool is_current = (step_pc == rt.script_pc);
                    if (is_current) {
                        ImGui::Text("> %s", step.label.c_str());
                    }
                    else {
                        ImGui::TextUnformatted(step.label.c_str());
                    }
                }
                ImGui::EndTable();
            }

            ImGui::SeparatorText("Video Viewport");
            if (!g_video.EnsureOpened(rt.video_ring_name)) {
                ImGui::TextUnformatted("Waiting for video ring attach...");
            }
            else {
                g_video.PollAndUpload(g_app.D3DDevice(), g_app.D3DContext());
                g_video.DrawImGui(ImGui::GetContentRegionAvail().x, 420.0f);
            }
        }
        else {
            ImGui::TextUnformatted("No runtime snapshot (session may not be active).");
            g_video.Reset();
        }
    }
    else {
        g_video.Reset();
    }

    ImGui::End();
}
