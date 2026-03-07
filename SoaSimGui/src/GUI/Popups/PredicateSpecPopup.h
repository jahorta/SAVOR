#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <span>
#include <future>

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include "Core/Memory/Soa/SoaAddrRegistry.h"
#include "Core/Memory/Soa/SoaAddrProgram.h"
#include "Core/Memory/Soa/SoaAddrProgramBuilder.h"
#include "Core/Memory/Soa/SoaAddrCatalog.h"

#include "Runner/Breakpoints/Predicate.h"
#include "DB/PredicateSpecRepo.h"
#include "DB/AddressProgramRepo.h"
#include "Runner/Breakpoints/BPRegistry.h"

#include "../../Components/ToastBus.h"
#include "../../Components/FutureQueue.h"
#include "../GuiCommon.h"
#include "../Widgets/SearchableCombo.h"
#include "../Widgets/SegmentedControl.h"
#include "../Widgets/BitmaskEditor.h"
#include "../Widgets/AddrProgramEditor.h"
#include "../Widgets/BreakpointPicker.h"
#include "../Widgets/InputTextMultilineWrap.h"

namespace soasim::ui {

    class PredicateSpecPopup {
    public:
        PredicateSpecPopup()
            : modal_id_("PredicateSpecPopup")
        {
            cache_addr_keys_();
            reset_draft_();
        }

        void Open() {
            reset_draft_();
            is_open = true;
            want_focus_ = true;
            ImGui::OpenPopup(modal_id_.c_str());
        }

        void Open(int64_t id) {
            reset_draft_();
            is_open = true;

            auto row = simcore::db::PredicateSpecRepo::Get(id);
            if (row.ok) {
                apply_predicate_row(row.value);
            }
            else {
                GuiToastBus::Error("Failed to load Predicate", row.error.message);
                // stay in new-mode with defaults
            }

            want_focus_ = true;
            ImGui::OpenPopup(modal_id_.c_str());
        }

        std::pair<bool, int64_t> Draw() {

            // If closed, nothing to draw.
            if (!is_open) return { false , 0 };

            // Make it a separate, non-dockable OS window; size/pos sane on first appear.
            ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse
                | ImGuiWindowFlags_AlwaysAutoResize
                | ImGuiWindowFlags_NoSavedSettings;

            // Center on first appear relative to main viewport (only as a starting point).
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSizeConstraints(ImVec2(720, 0), ImVec2(vp->WorkSize.x, vp->WorkSize.y));

            if (want_focus_) {
                ImGui::SetNextWindowFocus();   // brings this OS window/frontmost when it opens
            }

            if (ImGui::Begin("Predicate Spec##detached", &is_open, wf))
            {
                // On open, request focus/OS z-order so this window (and its popups) sit above main.
                if (want_focus_) { want_focus_ = false; }

                if (errors_.size() > 0) {
                    ImGui::SeparatorText("Validation Errors");
                    for (auto e : errors_) {
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "- %s", e.c_str());
                    }
                }

                ImGui::SeparatorText("Core");
                // Kind
                int kind_idx = (int)draft_.kind;
                widgets::Segmented("kind", VecPredKinds(), &kind_idx);
                draft_.kind = (simcore::pred::PredKind)kind_idx;

                // Width
                static const char* kWidths[] = { "1", "2", "4", "8" };
                if (ImGui::BeginCombo("Width (bytes)", kWidths[width_index_])) {
                    for (int i = 0; i < 4; ++i) {
                        bool sel = (i == width_index_);
                        if (ImGui::Selectable(kWidths[i], sel)) width_index_ = i;
                    }
                    ImGui::EndCombo();
                }
                draft_.width = (uint8_t)(1 << width_index_);

                ImGui::SeparatorText("Execution");
                // Named breakpoint picker (writes BPKey into required_bp)
                {
                    // cast storage is already BPKey-compatible (uint16)
                    BPKey tmp = static_cast<BPKey>(draft_.required_bp);
                    if (widgets::BreakpointPicker("Required Breakpoint", &tmp, bp_picker_)) {
                        draft_.required_bp = static_cast<uint16_t>(tmp);
                    }

                    // Small readout of the selected entry's PC
                    if (draft_.required_bp != 0) {
                        auto pc = bp::BPRegistry::pc(static_cast<BPKey>(draft_.required_bp));
                        const char* nm = bp::BPRegistry::name(static_cast<BPKey>(draft_.required_bp));
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s @ 0x%08X", (nm && nm[0]) ? nm : "(unknown)", pc);
                    }
                }

                ImGui::SeparatorText("Flags");
                ImGui::Checkbox("Active", &flag_active_);
                ImGui::SameLine();
                ImGui::Checkbox("Abort on fail", &flag_abort_);
                ImGui::SameLine();
                ImGui::Checkbox("Capture baseline", &flag_capture_);

                ImGui::SeparatorText("Match");
                if (ImGui::BeginTable("##match_row", 3, ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("LHS", ImGuiTableColumnFlags_WidthFixed, 280.0);
                    ImGui::TableSetupColumn("Compare", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                    ImGui::TableSetupColumn("RHS", ImGuiTableColumnFlags_WidthFixed, 280.0);
                    ImGui::TableNextRow();

                    // -------- LHS --------
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("LHS");
                    ImGui::SetNextItemWidth(-1);
                    static const char* kLHSOptions[] = { "Absolute Addr (Mem1)", "AddrKey", "AddrProgram" };
                    if (ImGui::BeginCombo("##lhs_src", kLHSOptions[lhs_index_])) {
                        for (int i = 0; i < 3; ++i) {
                            bool sel = (i == lhs_index_);
                            if (ImGui::Selectable(kLHSOptions[i], sel)) lhs_index_ = i;
                        }
                        ImGui::EndCombo();
                    }

                    ImGui::SetNextItemWidth(-1);
                    if (lhs_index_ == 0) {
                        // Absolute address
                        draft_.lhs_key.reset();
                        draft_.lhs_prog.clear(); draft_.lhs_prog_desc.clear();
                        ImGui::InputScalar("Absolute VA##lhs_va", ImGuiDataType_U32, &draft_.lhs_addr);
                    }
                    else if (lhs_index_ == 1) {
                        // AddrKey
                        draft_.lhs_prog.clear(); draft_.lhs_prog_desc.clear();
                        widgets::SearchableCombo("Key##lhs_key", addr_names_, &lhs_key_idx_, lhs_key_combo_);
                        draft_.lhs_key = (lhs_key_idx_ >= 0) ? std::optional(addr_keys_[lhs_key_idx_]) : std::nullopt;
                    }
                    else { // lhs_index_ == 2
                        // AddrProgram
                        draft_.lhs_key.reset();
                        lhs_prog_.Draw("Address program##lhs_prog", false);
                        draft_.lhs_prog = lhs_prog_.blob;
                        draft_.lhs_prog_desc = lhs_prog_.desc;
                    }

                    // -------- CMP --------
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted("Compare");
                    int cmp_idx = (int)draft_.cmp;
                    widgets::Segmented("##cmp_vert", VecCmpOps(), &cmp_idx, /*horizontal=*/false, /*two_per_row*/true);
                    draft_.cmp = (simcore::pred::CmpOp)cmp_idx;

                    // -------- RHS --------
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted("RHS");
                    ImGui::SetNextItemWidth(-1);
                    static const char* kRHSOptions[] = { "Immediate", "AddrKey", "AddrProgram" };
                    if (ImGui::BeginCombo("##rhs_src", kRHSOptions[rhs_index_])) {
                        for (int i = 0; i < 3; ++i) {
                            bool sel = (i == rhs_index_);
                            if (ImGui::Selectable(kRHSOptions[i], sel)) rhs_index_ = i;
                        }
                        ImGui::EndCombo();
                    }

                    ImGui::SetNextItemWidth(-1);
                    if (rhs_index_ == 0) {
                        // Immediate
                        draft_.rhs_key.reset();
                        draft_.rhs_prog.clear(); draft_.rhs_prog_desc.clear();
                        ImGui::InputScalar("Value##rhs_imm", ImGuiDataType_U64, &draft_.rhs_value);
                    }
                    else if (rhs_index_ == 1) {
                        // AddrKey
                        draft_.rhs_prog.clear(); draft_.rhs_prog_desc.clear();
                        widgets::SearchableCombo("Key##rhs_key", addr_names_, &rhs_key_idx_, rhs_key_combo_);
                        draft_.rhs_key = (rhs_key_idx_ >= 0) ? std::optional(addr_keys_[rhs_key_idx_]) : std::nullopt;
                    }
                    else { // rhs_index_ == 2
                        // AddrProgram
                        draft_.rhs_key.reset();
                        rhs_prog_.Draw("Address program##rhs_prog", false);
                        draft_.rhs_prog = rhs_prog_.blob;
                        draft_.rhs_prog_desc = rhs_prog_.desc;
                    }

                    ImGui::EndTable();
                }

                widgets::TurnMaskEditor("Turn mask", &draft_.turn_mask);

                if (ImGui::InputText("Name", &draft_.name) && draft_.name.size() > pred::PredNameLength) {
                    while (draft_.name.size() > pred::PredNameLength)
                        draft_.name.pop_back();
                }
                InputTextMultilineWordWrap("Description", &draft_.desc, ImVec2(480, 80));

                ImGui::Separator();
                if (ImGui::Button("Save")) {
                    assemble_flags_();
                    if (is_valid())
                    {
                        FutureQueue::Enqueue(
                            std::async(&PredicateSpecPopup::persist_, this),
                            [this](int64_t id) 
                            {
                                if (id == 0) GuiToastBus::Error("Failed to save Predicate");
                                else 
                                {
                                    GuiToastBus::Success("Predicate saved");
                                    saved_id = id;
                                }
                                is_open = false;
                            },
                            [](std::exception_ptr e) {
                                GuiToastBus::Error("Error saving Predicate");
                            }
                        );
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    is_open = false;
                }

                ImGui::End();
            }

            if (!is_open) {
                ImGui::CloseCurrentPopup();
                if (saved_id > 0) 
                    return { true, saved_id };
            }
            return { false , 0 };
        }

    private:
        struct SaveCtx {
            std::shared_ptr<SaveCtx> self;
            simcore::db::PredicateSpecRow row;
            int pending{ 0 };
            std::optional<int64_t> lhs_id;
            std::optional<int64_t> rhs_id;
        };

        std::string modal_id_;
        bool is_open{ false };
        bool want_focus_ = false;
        std::vector<std::string> errors_;
        int64_t saved_id;

        simcore::pred::Spec draft_{};
        int width_index_{ 2 }; // 4 bytes default

        bool flag_capture_{ true };
        bool flag_active_{ true };
        bool flag_abort_{ false };

        widgets::BreakpointPickerState bp_picker_;

        int  lhs_index_{ 0 };
        bool lhs_use_key_{ false };
        bool lhs_use_prog_{ false };
        int  lhs_key_idx_{ -1 };
        widgets::SearchableComboState lhs_key_combo_{};
        AddressProgramDraft lhs_prog_{};

        int  rhs_index_{ 0 };
        bool rhs_is_key_{ false };
        bool rhs_use_prog_{ false };
        int  rhs_key_idx_{ -1 };
        widgets::SearchableComboState rhs_key_combo_{};
        AddressProgramDraft rhs_prog_{};

        std::vector<std::string> addr_names_;
        std::vector<addr::AddrKey> addr_keys_;

        void reset_draft_() {
            draft_ = {};
            draft_.width = 4;
            draft_.cmp = simcore::pred::CmpOp::EQ;
            draft_.kind = simcore::pred::PredKind::ABS;
            draft_.turn_mask = 0xFFFFFFFFu;
            width_index_ = 2;
            flag_capture_ = true;
            flag_active_ = true;
            flag_abort_ = false;
            lhs_use_key_ = false; lhs_use_prog_ = false; lhs_key_idx_ = -1; lhs_prog_ = {};
            rhs_is_key_ = false;  rhs_use_prog_ = false;  rhs_key_idx_ = -1; rhs_prog_ = {};
            draft_.desc.clear();
            errors_.clear();
            lhs_index_ = 0;
            rhs_index_ = 0;
        }

        void apply_predicate_row(PredicateSpecRow row) {
            // Core
            draft_.required_bp = static_cast<uint16_t>(row.required_bp);
            draft_.kind = static_cast<simcore::pred::PredKind>(row.kind);
            draft_.cmp = static_cast<simcore::pred::CmpOp>(row.cmp_op);
            draft_.flags = static_cast<uint32_t>(row.flags);

            // Width -> index with fallback to 4
            int w = row.width;
            if (w == 1) { width_index_ = 0; draft_.width = 1; }
            else if (w == 2) { width_index_ = 1; draft_.width = 2; }
            else if (w == 4) { width_index_ = 2; draft_.width = 4; }
            else if (w == 8) { width_index_ = 3; draft_.width = 8; }
            else { width_index_ = 2; draft_.width = 4; }

            // Flags -> checkboxes
            flag_capture_ = draft_.has_flag(simcore::pred::PredFlag::CaptureBaseline);
            flag_active_ = draft_.has_flag(simcore::pred::PredFlag::Active);
            flag_abort_ = draft_.has_flag(simcore::pred::PredFlag::AbortOnFail);

            // Common DB fields
            draft_.turn_mask = static_cast<uint32_t>(row.turn_mask);
            draft_.name = row.name;
            draft_.desc = row.description;

            // -------- LHS --------
            draft_.lhs_addr = static_cast<uint32_t>(row.lhs_addr);
            if (draft_.has_flag(simcore::pred::PredFlag::LhsIsProg)) {
                lhs_index_ = 2;
                lhs_prog_ = {};
                // Keep program mode even if fetch fails
                if (row.lhs_prog_id) {
                    auto pr = simcore::db::AddressProgramRepo::Get(*row.lhs_prog_id);
                    if (pr.ok) {
                        lhs_prog_.blob = pr.value.prog_bytes;
                        lhs_prog_.desc = pr.value.description;
                        draft_.lhs_prog = lhs_prog_.blob;
                        draft_.lhs_prog_desc = lhs_prog_.desc;
                    }
                }
                draft_.lhs_key.reset();
            }
            else if (draft_.has_flag(simcore::pred::PredFlag::LhsIsKey)) {
                // Key mode; downgrade to absolute if missing
                lhs_index_ = 1;
                if (row.lhs_key) {
                    auto key = static_cast<addr::AddrKey>(*row.lhs_key);
                    if (addr::Registry::exists(key)) {
                        draft_.lhs_key = key;
                        // compute lhs_key_idx_
                        lhs_key_idx_ = -1;
                        for (int i = 0; i < (int)addr_keys_.size(); ++i) { if (addr_keys_[i] == key) { lhs_key_idx_ = i; break; } }
                    }
                    else {
                        lhs_index_ = 0;
                        draft_.lhs_key.reset();
                    }
                }
                else {
                    lhs_index_ = 0;
                    draft_.lhs_key.reset();
                }
                lhs_prog_ = {};
            }
            else {
                lhs_index_ = 0;
                draft_.lhs_key.reset();
                lhs_prog_ = {};
            }

            // -------- RHS --------
            draft_.rhs_value = static_cast<uint64_t>(row.rhs_value);
            if (draft_.has_flag(simcore::pred::PredFlag::RhsIsProg)) {
                rhs_index_ = 2;
                rhs_prog_ = {};
                if (row.rhs_prog_id) {
                    auto pr = simcore::db::AddressProgramRepo::Get(*row.rhs_prog_id);
                    // Keep program mode even if fetch fails
                    if (pr.ok) {
                        rhs_prog_.blob = pr.value.prog_bytes;
                        rhs_prog_.desc = pr.value.description;
                        draft_.rhs_prog = rhs_prog_.blob;
                        draft_.rhs_prog_desc = rhs_prog_.desc;
                    }
                }
                draft_.rhs_key.reset();
            }
            else if (draft_.has_flag(simcore::pred::PredFlag::RhsIsKey)) {
                rhs_index_ = 1;
                if (row.rhs_key) {
                    auto key = static_cast<addr::AddrKey>(*row.rhs_key);
                    if (addr::Registry::exists(key)) {
                        draft_.rhs_key = key;
                        rhs_key_idx_ = -1;
                        for (int i = 0; i < (int)addr_keys_.size(); ++i) { if (addr_keys_[i] == key) { rhs_key_idx_ = i; break; } }
                    }
                    else {
                        rhs_index_ = 0;
                        draft_.rhs_key.reset();
                    }
                }
                else {
                    rhs_index_ = 0;
                    draft_.rhs_key.reset();
                }
                rhs_prog_ = {};
            }
            else {
                rhs_index_ = 0;
                draft_.rhs_key.reset();
                rhs_prog_ = {};
            }
        }

        void cache_addr_keys_() {
            addr_names_.clear();
            addr_keys_.clear();
            auto all = addr::Registry::all();
            addr_names_.reserve(all.size());
            addr_keys_.reserve(all.size());
            for (auto& r : all) {
                addr_names_.push_back(r.name);
                addr_keys_.push_back(r.key);
            }
        }

        bool is_valid() {
            errors_.clear();

            if (draft_.required_bp == 0) errors_.emplace_back("Required breakpoint is not set");

            if (draft_.has_flag(simcore::pred::PredFlag::LhsIsProg) && draft_.lhs_prog.empty()) errors_.emplace_back("Lhs Program is empty");
            else if (draft_.has_flag(simcore::pred::PredFlag::LhsIsKey))
            {
                if (!draft_.lhs_key.has_value()) errors_.emplace_back("Lhs Key is not set");
                else if (!addr::Registry::exists(*draft_.lhs_key)) errors_.emplace_back("Lhs Key does not exist");
            }
            else if (!(0x80000000u <= draft_.lhs_addr <= 0x81ffffffu)) errors_.emplace_back("Lhs Addr should be between 0x80000000 and 0x81ffffff");

            if (draft_.has_flag(simcore::pred::PredFlag::RhsIsProg) && draft_.rhs_prog.empty()) errors_.emplace_back("Rhs Program is empty");
            else if (draft_.has_flag(simcore::pred::PredFlag::RhsIsKey))
            {
                if (!draft_.rhs_key.has_value()) errors_.emplace_back("Rhs Key is not set");
                else if (!addr::Registry::exists(*draft_.rhs_key)) errors_.emplace_back("Rhs Key does not exist");
            }

            return errors_.size() == 0;
        }

        void assemble_flags_() {
            draft_.flags = 0;
            if (flag_capture_) draft_.set_flag(simcore::pred::PredFlag::CaptureBaseline);
            if (flag_active_)  draft_.set_flag(simcore::pred::PredFlag::Active);
            if (flag_abort_)   draft_.set_flag(simcore::pred::PredFlag::AbortOnFail);
            
            switch (lhs_index_) {
            case 1: draft_.set_flag(simcore::pred::PredFlag::LhsIsKey); break;
            case 2: draft_.set_flag(simcore::pred::PredFlag::LhsIsProg); break;
            }
            
            switch (rhs_index_) {
            case 1: draft_.set_flag(simcore::pred::PredFlag::RhsIsKey); break;
            case 2: draft_.set_flag(simcore::pred::PredFlag::RhsIsProg); break;
            }
        }

        int64_t persist_() {
            auto row = to_row_(draft_);

            if (draft_.lhs_prog.size()) {
                auto r = simcore::db::AddressProgramRepo::Ensure(
                    (int32_t)addrprog::PROG_VERSION,
                    draft_.lhs_prog, std::nullopt, std::nullopt, std::nullopt,
                    draft_.lhs_prog_desc.empty() ? std::string("lhs program") : draft_.lhs_prog_desc);
                if (!r.ok) {
                    GuiToastBus::Error("Failed to save LHS program", r.error.message);
                    return 0;
                }
                row.lhs_prog_id = r.value;
            }

            if (draft_.rhs_prog.size()) {
                auto r = simcore::db::AddressProgramRepo::Ensure(
                    (int32_t)addrprog::PROG_VERSION,
                    draft_.rhs_prog, std::nullopt, std::nullopt, std::nullopt,
                    draft_.rhs_prog_desc.empty() ? std::string("rhs program") : draft_.rhs_prog_desc);
                if (!r.ok) {
                    GuiToastBus::Error("Failed to save RHS program", r.error.message);
                    return 0;
                }
                row.rhs_prog_id = r.value;
            }

            auto pred_id = simcore::db::PredicateSpecRepo::EnsureByFingerprint(row);
            if (!pred_id.ok) { GuiToastBus::Error("Failed to save predicate", pred_id.error.message); return 0; }
            GuiToastBus::Success("Predicate saved", std::to_string(pred_id.value));
            return pred_id.value;
        }

        static simcore::db::PredicateSpecRow to_row_(const simcore::pred::Spec& s) {
            simcore::db::PredicateSpecRow r{};
            r.spec_version = (int32_t)simcore::pred::SPEC_VERSION;
            r.required_bp = (int32_t)s.required_bp;
            r.kind = (int32_t)s.kind;
            r.width = (int32_t)(s.width ? s.width : 4);
            r.cmp_op = (int32_t)s.cmp;
            r.flags = (int32_t)s.flags;
            r.lhs_addr = (int64_t)s.lhs_addr;
            if (s.lhs_key) r.lhs_key = (int32_t)*s.lhs_key;
            r.rhs_value = (int64_t)s.rhs_value;
            if (s.rhs_key) r.rhs_key = (int32_t)*s.rhs_key;
            r.turn_mask = (int32_t)(s.turn_mask ? s.turn_mask : 0xFFFFFFFFu);
            r.name = s.name;
            r.description = s.desc;
            r.fingerprint = simcore::pred::fingerprint(s);
            return r;
        }
    };

} // namespace soasim::ui
