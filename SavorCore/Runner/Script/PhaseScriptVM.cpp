#include "PhaseScriptVM.h"
#include <algorithm>

#include "../../Phases/Programs/BattleMacroProbe/BattleMacroProbePayload.h"
#include "../../Phases/Programs/BattleRunner/BattleRunnerPayload.h"
#include "../../Core/Memory/Soa/Battle/BattleContextCodec.h"
#include "../../Core/Memory/MemView.h"
#include "../../Core/Memory/Soa/SoaAddrProgram.h"
#include "../../Core/Memory/Soa/SoaAddrRegistry.h"
#include "../Breakpoints/Predicate.h"
#include "../../Core/Input/SoaBattle/PlanWriter.h"
#include "../../Core/Input/SoaBattle/ActionLibrary.h"
#include "../../Core/Input/InputPlanFmt.h"
#include "../../Core/Input/BattleInputTraceBlob.h"
#include "../../Core/Memory/IKeyReader.h"
#include "../../Core/Memory/DerivedBase.h"
#include "../../Core/Memory/Soa/Battle/DerivedBattleBuffer.h"
#include "../../Core/Memory/KeyHostRouter.h"
#include "../Breakpoints/BpRegistry.h"
#include "../IPC/Wire.h"
#include "ScriptProgress.h"
#include "../../Utils/IniDoc.h"
#include <thread>
#include <sstream>
#include <cstring>

namespace {
    static uint16_t read_u16_le(const char* ptr) {
        return static_cast<uint16_t>(static_cast<unsigned char>(ptr[0]))
            | static_cast<uint16_t>(static_cast<unsigned char>(ptr[1]) << 8);
    }

    static bool record_has_baseline_bp(const std::string& table_blob, const savor::pred::PredicateRecord& record, const uint32_t bp_key) {
        if (record.baseline_bps_offset == 0 || bp_key == 0) {
            return false;
        }
        const auto offset = static_cast<size_t>(record.baseline_bps_offset);
        if (offset + sizeof(uint16_t) > table_blob.size()) {
            return false;
        }
        const char* ptr = table_blob.data() + offset;
        const uint16_t count = read_u16_le(ptr);
        ptr += sizeof(uint16_t);
        if (offset + sizeof(uint16_t) + static_cast<size_t>(count) * sizeof(uint16_t) > table_blob.size()) {
            return false;
        }
        for (uint16_t i = 0; i < count; ++i) {
            if (read_u16_le(ptr + i * sizeof(uint16_t)) == bp_key) {
                return true;
            }
        }
        return false;
    }

    static void append_record_baseline_bps(const std::string& table_blob, const savor::pred::PredicateRecord& record, std::vector<uint32_t>& bp_keys) {
        if (record.baseline_bps_offset == 0) {
            return;
        }
        const auto offset = static_cast<size_t>(record.baseline_bps_offset);
        if (offset + sizeof(uint16_t) > table_blob.size()) {
            return;
        }
        const char* ptr = table_blob.data() + offset;
        const uint16_t count = read_u16_le(ptr);
        ptr += sizeof(uint16_t);
        if (offset + sizeof(uint16_t) + static_cast<size_t>(count) * sizeof(uint16_t) > table_blob.size()) {
            return;
        }
        for (uint16_t i = 0; i < count; ++i) {
            bp_keys.push_back(read_u16_le(ptr + i * sizeof(uint16_t)));
        }
    }

    static const BPAddr* find_hit_bp(
        const BreakpointMap& bpmap,
        const std::vector<BPKey>& canonical_bp_keys,
        const std::vector<BPKey>& predicate_bp_keys,
        uint32_t pc)
    {
        for (auto k : canonical_bp_keys) {
            if (const auto* e = bpmap.find(k); e && e->pc == pc) return e;
        }
        for (auto k : predicate_bp_keys) {
            if (const auto* e = bpmap.find(k); e && e->pc == pc) return e;
        }
        return nullptr;
    }

    static const BPAddr* find_hit_bp_in_keys(
        const BreakpointMap& bpmap,
        const std::vector<BPKey>& keys,
        uint32_t pc)
    {
        for (auto k : keys) {
            if (const auto* e = bpmap.find(k); e && e->pc == pc) return e;
        }
        return nullptr;
    }

    static const BPAddr* find_hit_bp(
        const BreakpointMap& bpmap,
        const std::vector<BPKey>& canonical_bp_keys,
        const std::vector<BPKey>& reserved_bp_keys,
        const std::vector<BPKey>& predicate_bp_keys,
        uint32_t pc)
    {
        if (const auto* e = find_hit_bp_in_keys(bpmap, canonical_bp_keys, pc)) return e;
        if (const auto* e = find_hit_bp_in_keys(bpmap, reserved_bp_keys, pc)) return e;
        return find_hit_bp_in_keys(bpmap, predicate_bp_keys, pc);
    }

    static const char* stable_bp_id_or_empty(const BPAddr* bp)
    {
        return bp != nullptr && bp->stable_id != nullptr ? bp->stable_id : "";
    }

    inline bool read_via_addrprog(savor::DolphinWrapper& host,
        const savor::IDerivedBuffer* derived,
        const std::string& table_and_blob,
        uint32_t prog_off,
        uint8_t width,
        uint64_t& out_bits)
    {
        if (prog_off == 0) return false;

        const uint8_t* base = reinterpret_cast<const uint8_t*>(table_and_blob.data());
        const size_t   sz = table_and_blob.size();
        return addrprog::read_value(base, sz, prog_off, host, derived, width, out_bits);
    }
}

namespace savor {
    namespace {
        std::string ps_cmp_to_string(PSCmp cmp) {
            switch (cmp) {
            case PSCmp::EQ: return "EQ";
            case PSCmp::NE: return "NE";
            case PSCmp::LT: return "LT";
            case PSCmp::LE: return "LE";
            case PSCmp::GT: return "GT";
            case PSCmp::GE: return "GE";
            default: return "?";
            }
        }

        std::string key_desc(savor::context::key::KeyId key) {
            const std::string_view name = savor::context::key::name_for_id(key);
            if (!name.empty()) return std::string(name);
            return std::to_string(static_cast<uint32_t>(key));
        }

        std::string bp_key_list_desc(const std::vector<BPKey>& keys) {
            std::ostringstream out;
            for (size_t i = 0; i < keys.size(); ++i) {
                if (i != 0) out << ",";
                out << static_cast<uint32_t>(keys[i]);
            }
            return out.str();
        }

        DolphinWrapper::MemoryWatchpointAccess capture_access_to_wrapper_access(
            savor::capture::WatchpointAccess access)
        {
            switch (access) {
            case savor::capture::WatchpointAccess::Read:
                return DolphinWrapper::MemoryWatchpointAccess::Read;
            case savor::capture::WatchpointAccess::Access:
                return DolphinWrapper::MemoryWatchpointAccess::Access;
            case savor::capture::WatchpointAccess::Write:
            default:
                return DolphinWrapper::MemoryWatchpointAccess::Write;
            }
        }

        std::vector<DolphinWrapper::MemoryWatchpointSpec> active_capture_watchpoints_to_wrapper_specs(
            const std::vector<savor::capture::LiveCheckpointCapture::ActiveMemoryWatchpointSpec>& active_watchpoints)
        {
            std::vector<DolphinWrapper::MemoryWatchpointSpec> out;
            out.reserve(active_watchpoints.size());
            for (const auto& watchpoint : active_watchpoints) {
                out.push_back(DolphinWrapper::MemoryWatchpointSpec{
                    .id = watchpoint.id,
                    .address = watchpoint.address,
                    .size = watchpoint.size,
                    .access = capture_access_to_wrapper_access(watchpoint.access),
                });
            }
            return out;
        }

        const char* debug_stop_kind_to_string(DolphinWrapper::DebugStopKind kind)
        {
            switch (kind) {
            case DolphinWrapper::DebugStopKind::PcBreakpoint: return "pc_breakpoint";
            case DolphinWrapper::DebugStopKind::Memcheck: return "memcheck";
            case DolphinWrapper::DebugStopKind::PcBreakpointAndMemcheck: return "pc_breakpoint_and_memcheck";
            case DolphinWrapper::DebugStopKind::None:
            default:
                return "none";
            }
        }
    }

    PhaseScriptVM::PhaseScriptVM(savor::DolphinWrapper& host, const BreakpointMap& bpmap)
        : host_(host), bpmap_(bpmap) {
    }

    void PhaseScriptVM::SetVisualDebugMode(bool enabled)
    {
        visual_debug_mode_ = enabled;
        if (!enabled) {
            visual_debug_paused_.store(false, std::memory_order_release);
            visual_debug_vm_step_budget_.store(0, std::memory_order_release);
        }
    }

    void PhaseScriptVM::SetVisualDebugPaused(bool paused)
    {
        visual_debug_paused_.store(paused, std::memory_order_release);
        if (!paused) {
            visual_debug_vm_step_budget_.store(0, std::memory_order_release);
        }
    }

    void PhaseScriptVM::StepVisualDebugVmOnce()
    {
        visual_debug_paused_.store(true, std::memory_order_release);
        visual_debug_vm_step_budget_.fetch_add(1, std::memory_order_acq_rel);
    }

    bool PhaseScriptVM::IsVisualDebugVmPaused() const
    {
        return visual_debug_paused_.load(std::memory_order_acquire);
    }

    bool PhaseScriptVM::IsRunUntilBpActive() const
    {
        return run_until_bp_active_.load(std::memory_order_acquire);
    }

    void PhaseScriptVM::wait_for_visual_debug_gate()
    {
        if (!visual_debug_mode_) return;
        for (;;) {
            if (!visual_debug_paused_.load(std::memory_order_acquire)) {
                return;
            }
            uint32_t budget = visual_debug_vm_step_budget_.load(std::memory_order_acquire);
            if (budget > 0) {
                if (visual_debug_vm_step_budget_.compare_exchange_strong(budget, budget - 1, std::memory_order_acq_rel)) {
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    bool PhaseScriptVM::save_snapshot() {
        return host_.saveStateToBuffer(snapshot_);
    }

    bool PhaseScriptVM::load_snapshot() {
        return host_.loadStateFromBuffer(snapshot_);
    }

    void PhaseScriptVM::arm_bps_once() {
        if (armed_) return;
        std::vector<uint32_t> pcs;
        pcs.reserve(canonical_bp_keys_.size() + reserved_bp_keys_.size());
        const auto append_unique_pc = [&](BPKey k) {
            if (const auto* e = bpmap_.find(k)) {
                if (std::find(pcs.begin(), pcs.end(), e->pc) == pcs.end()) {
                    pcs.push_back(e->pc);
                }
            }
        };
        for (const auto& k : canonical_bp_keys_) append_unique_pc(k);
        for (const auto& k : reserved_bp_keys_) append_unique_pc(k);
        if (!pcs.empty()) {
            host_.armPcBreakpoints(pcs);
            armed_pcs_ = pcs;
            restore_canonical_breakpoint_scope();
        }
        armed_ = true;
    }

    std::vector<uint32_t> PhaseScriptVM::capture_pcs() const {
        return capture_ && capture_->active() ? capture_->pcs() : std::vector<uint32_t>{};
    }

    void PhaseScriptVM::append_capture_pcs(std::vector<uint32_t>& pcs) const {
        for (const auto pc : capture_pcs()) {
            if (std::find(pcs.begin(), pcs.end(), pc) == pcs.end()) {
                pcs.push_back(pc);
            }
        }
    }

    void PhaseScriptVM::disarm_exhausted_capture_pcs() {
        if (!capture_ || !capture_->active()) {
            return;
        }

        auto exhausted = capture_->take_newly_exhausted_pcs();
        if (exhausted.empty()) {
            return;
        }

        const auto pc_is_program_armed = [&](uint32_t pc) {
            if (std::find(armed_pcs_.begin(), armed_pcs_.end(), pc) != armed_pcs_.end()) {
                return true;
            }
            for (const auto key : predicate_bp_keys_) {
                if (const auto* e = bpmap_.find(key); e != nullptr && e->pc == pc) {
                    return true;
                }
            }
            for (const auto key : macro_enabled_bp_keys_) {
                if (const auto* e = bpmap_.find(key); e != nullptr && e->pc == pc) {
                    return true;
                }
            }
            return false;
        };

        std::vector<uint32_t> to_disarm;
        for (const auto pc : exhausted) {
            capture_armed_pcs_.erase(
                std::remove(capture_armed_pcs_.begin(), capture_armed_pcs_.end(), pc),
                capture_armed_pcs_.end());
            if (!pc_is_program_armed(pc)
                && std::find(to_disarm.begin(), to_disarm.end(), pc) == to_disarm.end()) {
                to_disarm.push_back(pc);
            }
        }
        if (!to_disarm.empty()) {
            host_.disarmPcBreakpoints(to_disarm);
        }
    }

    bool PhaseScriptVM::configure_capture_from_context(const PSContext& ctx, PSResult& result) {
        std::string profile_path;
        std::string output_path;
        const bool has_profile = ctx.get<std::string>(savor::context::key::core::CAPTURE_PROFILE_PATH, profile_path);
        const bool has_output = ctx.get<std::string>(savor::context::key::core::CAPTURE_OUTPUT_PATH, output_path);
        if (!has_profile && !has_output) {
            reset_capture_session(true);
            return true;
        }
        if (!has_profile || !has_output || profile_path.empty() || output_path.empty()) {
            result.ctx = ctx;
            SCLOGW("[capture] profile and output paths are both required");
            return false;
        }
        reset_capture_session(false);
        if (!capture_) {
            capture_ = std::make_unique<savor::capture::LiveCheckpointCapture>();
        }
        std::string error;
        if (!capture_->start(profile_path, output_path, &error)) {
            result.ctx = ctx;
            SCLOGW("[capture] start failed profile=%s output=%s error=%s",
                profile_path.c_str(), output_path.c_str(), error.c_str());
            return false;
        }

        if (!arm_capture_memory_watchpoints(savor::capture::WatchpointScope::Normal)) {
            result.ctx = ctx;
            SCLOGW("[capture] memory watchpoint arm failed profile=%s", profile_path.c_str());
            capture_->stop();
            return false;
        }

        arm_capture_breakpoints();
        SCLOGI("[capture] active profile=%s output=%s pc_count=%zu watchpoint_count=%zu",
            profile_path.c_str(),
            output_path.c_str(),
            capture_armed_pcs_.size(),
            capture_->memory_watchpoints().size());
        return true;
    }

    void PhaseScriptVM::arm_capture_breakpoints() {
        if (!capture_ || !capture_->active()) {
            capture_armed_pcs_.clear();
            return;
        }
        capture_armed_pcs_ = capture_->pcs();
        if (!capture_armed_pcs_.empty()) {
            host_.armPcBreakpoints(capture_armed_pcs_);
        }
        restore_canonical_breakpoint_scope();
    }

    bool PhaseScriptVM::arm_capture_memory_watchpoints(savor::capture::WatchpointScope scope) {
        if (!capture_ || !capture_->active()) {
            return true;
        }
        auto active_watchpoints = capture_->static_memory_watchpoints(scope);
        const auto memory_watchpoints =
            active_capture_watchpoints_to_wrapper_specs(active_watchpoints);
        if (memory_watchpoints.empty()) {
            host_.clearMemoryWatchpoints();
            capture_->set_active_memory_watchpoints({});
            return true;
        }
        host_.clearMemoryWatchpoints();
        if (!host_.armMemoryWatchpoints(memory_watchpoints)) {
            capture_->set_active_memory_watchpoints({});
            return false;
        }
        capture_->set_active_memory_watchpoints(std::move(active_watchpoints));
        return true;
    }

    void PhaseScriptVM::clear_capture_memory_watchpoints() {
        host_.clearMemoryWatchpoints();
        if (capture_ && capture_->active()) {
            capture_->set_active_memory_watchpoints({});
        }
    }

    void PhaseScriptVM::reset_capture_session(bool restore_scope) {
        const auto pc_is_program_armed = [&](uint32_t pc) {
            if (std::find(armed_pcs_.begin(), armed_pcs_.end(), pc) != armed_pcs_.end()) {
                return true;
            }
            for (const auto key : predicate_bp_keys_) {
                if (const auto* e = bpmap_.find(key); e != nullptr && e->pc == pc) {
                    return true;
                }
            }
            for (const auto key : macro_enabled_bp_keys_) {
                if (const auto* e = bpmap_.find(key); e != nullptr && e->pc == pc) {
                    return true;
                }
            }
            return false;
        };

        std::vector<uint32_t> to_disarm;
        for (const auto pc : capture_armed_pcs_) {
            if (!pc_is_program_armed(pc)) {
                to_disarm.push_back(pc);
            }
        }
        if (!to_disarm.empty()) {
            host_.disarmPcBreakpoints(to_disarm);
        }
        if (capture_) {
            capture_->stop();
        }
        host_.clearMemoryWatchpoints();
        capture_armed_pcs_.clear();
        if (restore_scope) {
            restore_canonical_breakpoint_scope();
        }
    }

    bool PhaseScriptVM::capture_current_hit(
        uint32_t pc,
        PSContext& ctx,
        savor::capture::WatchpointScope scope) {
        if (!capture_ || !capture_->active() || !capture_->contains_pc(pc)) {
            return true;
        }
        std::string error;
        if (!capture_->capture_hit(host_, pc, &error)) {
            ctx[savor::context::key::core::WORKER_ERROR] = static_cast<uint32_t>(WERR_UnknownError);
            SCLOGW("[capture] checkpoint write failed pc=%08X error=%s", pc, error.c_str());
            return false;
        }
        auto dynamic_watchpoints = capture_->derive_dynamic_memory_watchpoints(host_, pc, scope);
        if (!dynamic_watchpoints.empty()) {
            capture_->append_active_memory_watchpoints(std::move(dynamic_watchpoints));
            const auto specs =
                active_capture_watchpoints_to_wrapper_specs(capture_->active_memory_watchpoints());
            host_.clearMemoryWatchpoints();
            if (!host_.armMemoryWatchpoints(specs)) {
                ctx[savor::context::key::core::WORKER_ERROR] = static_cast<uint32_t>(WERR_UnknownError);
                SCLOGW("[capture] dynamic memory watchpoint arm failed pc=%08X count=%zu",
                    pc,
                    specs.size());
                return false;
            }
        }
        disarm_exhausted_capture_pcs();
        return true;
    }

    void PhaseScriptVM::step_past_capture_only_breakpoint(uint32_t timeout_ms, const RunUntilBpSpec& spec) {
        host_.setEnabledPcBreakpointsOnly({});
        (void)host_.stepOneOpcodeBlocking(static_cast<int>(timeout_ms));
        if (spec.expected_only_scope) {
            std::vector<uint32_t> pcs;
            pcs.reserve(spec.expected_bp_keys.size() + capture_armed_pcs_.size());
            for (const auto expected_bp : spec.expected_bp_keys) {
                if (const auto* e = bpmap_.find(expected_bp)) {
                    if (std::find(pcs.begin(), pcs.end(), e->pc) == pcs.end()) {
                        pcs.push_back(e->pc);
                    }
                }
            }
            append_capture_pcs(pcs);
            host_.setEnabledPcBreakpointsOnly(pcs);
        } else {
            restore_canonical_breakpoint_scope();
        }
    }

    bool PhaseScriptVM::step_past_capture_only_memory_watchpoint(uint32_t timeout_ms, const RunUntilBpSpec& spec) {
        std::vector<savor::capture::LiveCheckpointCapture::ActiveMemoryWatchpointSpec> active_watchpoints;
        if (capture_ && capture_->active()) {
            active_watchpoints = capture_->active_memory_watchpoints();
        }

        host_.clearMemoryWatchpoints();
        host_.setEnabledPcBreakpointsOnly({});
        (void)host_.stepOneOpcodeBlocking(static_cast<int>(timeout_ms));

        if (!active_watchpoints.empty()) {
            if (!host_.armMemoryWatchpoints(active_capture_watchpoints_to_wrapper_specs(active_watchpoints))) {
                SCLOGW("[capture] memory watchpoint rearm failed after capture-only memcheck");
                return false;
            }
        }

        if (spec.expected_only_scope) {
            std::vector<uint32_t> pcs;
            pcs.reserve(spec.expected_bp_keys.size() + capture_armed_pcs_.size());
            for (const auto expected_bp : spec.expected_bp_keys) {
                if (const auto* e = bpmap_.find(expected_bp)) {
                    if (std::find(pcs.begin(), pcs.end(), e->pc) == pcs.end()) {
                        pcs.push_back(e->pc);
                    }
                }
            }
            append_capture_pcs(pcs);
            host_.setEnabledPcBreakpointsOnly(pcs);
        } else {
            restore_canonical_breakpoint_scope();
        }
        return true;
    }

    void PhaseScriptVM::restore_canonical_breakpoint_scope() {
        std::vector<uint32_t> enabled_pcs;
        enabled_pcs.reserve(canonical_bp_keys_.size() + predicate_bp_keys_.size());
        const auto append_pc = [&](BPKey key) {
            if (const auto* e = bpmap_.find(key)) {
                if (std::find(enabled_pcs.begin(), enabled_pcs.end(), e->pc) == enabled_pcs.end()) {
                    enabled_pcs.push_back(e->pc);
                }
            }
        };
        for (const auto& k : canonical_bp_keys_) {
            append_pc(k);
        }
        for (const auto& k : predicate_bp_keys_) {
            append_pc(k);
        }
        append_capture_pcs(enabled_pcs);
        host_.setEnabledPcBreakpointsOnly(enabled_pcs);
    }

    void PhaseScriptVM::begin_macro_breakpoint_scope() {
        disable_macro_step_breakpoint();
        host_.setEnabledPcBreakpointsOnly({});
        macro_breakpoint_scope_active_ = true;
        SCLOGI("[battle-macro-scope] begin");
    }

    void PhaseScriptVM::enable_macro_step_breakpoint(BPKey key) {
        disable_macro_step_breakpoint();
        if (const auto* e = bpmap_.find(key)) {
            std::vector<uint32_t> enabled{ e->pc };
            append_capture_pcs(enabled);
            host_.setEnabledPcBreakpointsOnly(enabled);
            macro_enabled_bp_keys_.push_back(key);
            SCLOGI("[battle-macro-scope] enable key=%u pc=%08X",
                static_cast<uint32_t>(key),
                e->pc);
        } else {
            SCLOGW("[battle-macro-scope] enable_missing key=%u", static_cast<uint32_t>(key));
        }
    }

    void PhaseScriptVM::disable_macro_step_breakpoint() {
        if (!macro_enabled_bp_keys_.empty()) {
            host_.setEnabledPcBreakpointsOnly({});
        }
        for (const auto key : macro_enabled_bp_keys_) {
            if (const auto* e = bpmap_.find(key)) {
                SCLOGI("[battle-macro-scope] disable key=%u pc=%08X",
                    static_cast<uint32_t>(key),
                    e->pc);
            }
        }
        macro_enabled_bp_keys_.clear();
    }

    void PhaseScriptVM::end_macro_breakpoint_scope() {
        if (!macro_breakpoint_scope_active_ && macro_enabled_bp_keys_.empty()) {
            return;
        }
        disable_macro_step_breakpoint();
        clear_capture_memory_watchpoints();
        macro_breakpoint_scope_active_ = false;
        restore_canonical_breakpoint_scope();
        SCLOGI("[battle-macro-scope] end");
    }

    bool PhaseScriptVM::init(const PSInit& init, const PhaseScript& program)
    {
        init_ = init;
        prog_ = program;
        host_.clearMemoryWatchpoints();

        switch (init_.derived_buffer_type) {
        case DK_Battle: derived_ = std::make_unique<savor::DerivedBattleBuffer>(); break;
            // case 2: derived_ = std::make_unique<savor::DerivedExploreBuffer>(); break; // future
        default: derived_.reset(); break;
        }

        SCLOGDX(SC_TAGS("vm", "init"), "[VM] init begin sav=%s timeout=%u", init.savestate_path.c_str(), init.default_timeout_ms);

        // Disarm any previously armed set (enables program swapping)
        if (armed_ && !armed_pcs_.empty()) {
            macro_breakpoint_scope_active_ = false;
            macro_enabled_bp_keys_.clear();
            host_.disarmPcBreakpoints(armed_pcs_);
            armed_pcs_.clear();
        }
        armed_ = false;

        // Optional savestate (allow empty path for boot-based phases)
        if (!init_.savestate_path.empty()) {
            if (!host_.loadSavestate(init_.savestate_path.c_str()))
                return false;
        }

        // Update BP keys and arm once. Reserved keys stay disabled unless a specialized op enables them.
        canonical_bp_keys_ = prog_.canonical_bp_keys;
        reserved_bp_keys_ = prog_.reserved_bp_keys;

        SCLOGDX(
            SC_TAGS("vm", "breakpoint"),
            "[VM] attach bp count=%zu reserved=%zu",
            program.canonical_bp_keys.size(),
            program.reserved_bp_keys.size());
        arm_bps_once();

        // Capture a snapshot to use as the per-job baseline
        const bool snapshot_ok = save_snapshot();
        if (snapshot_ok) SCLOGDX(SC_TAGS("vm", "init"), "[VM] init ok");
        return snapshot_ok;
    }

    bool PhaseScriptVM::compare_u32(uint32_t lhs, PSCmp cmp, uint32_t rhs) const {
        switch (cmp) {
        case PSCmp::EQ: return lhs == rhs;
        case PSCmp::NE: return lhs != rhs;
        case PSCmp::LT: return lhs < rhs;
        case PSCmp::LE: return lhs <= rhs;
        case PSCmp::GT: return lhs > rhs;
        case PSCmp::GE: return lhs >= rhs;
        default: return false;
        }
    }

    void PhaseScriptVM::jump_to_label_if_exists(const std::string& label, const std::unordered_map<std::string, size_t>& label_vm_pc_map, size_t& vm_pc, std::string& section) const {
        auto it = label_vm_pc_map.find(label);
        if (it != label_vm_pc_map.end()) {
            section = label;
            vm_pc = it->second;
        }
    }

    bool PhaseScriptVM::op_arm_phase_bps_once() { arm_bps_once(); return true; }
    bool PhaseScriptVM::op_load_snapshot(PSContext& ctx) { if (!load_snapshot()) return false; ctx[savor::context::key::core::VI_FIRST] = (uint32_t)(host_.getViFieldCountApprox() & 0xFFFFFFFFull); return true; }
    bool PhaseScriptVM::op_capture_snapshot() { return save_snapshot(); }
    bool PhaseScriptVM::op_reboot_core(PSResult& result, PSContext& ctx) {
        std::string iso_path{};
        if (!ctx.get(savor::context::key::core::GAME_ISO_PATH, iso_path)) {
            ctx[savor::context::key::core::WORKER_ERROR] = (uint32_t)32;
            result.ctx = ctx;
            return false;
        }
        host_.clearMemoryWatchpoints();
        host_.clearAllPcBreakpoints();
        host_.loadGame(iso_path);
        host_.ConfigurePortsStandardPadP1();
        armed_ = false;
        armed_pcs_.clear();
        arm_bps_once();
        if (capture_ && capture_->active()) {
            if (!arm_capture_memory_watchpoints(savor::capture::WatchpointScope::Normal)) {
                ctx[savor::context::key::core::WORKER_ERROR] = static_cast<uint32_t>(WERR_UnknownError);
                result.ctx = ctx;
                return false;
            }
            arm_capture_breakpoints();
        }
        return true;
    }
    void PhaseScriptVM::op_label() const {}
    void PhaseScriptVM::op_goto(const PSOp& op, const std::unordered_map<std::string, size_t>& label_vm_pc_map, size_t& vm_pc, std::string& section) const { jump_to_label_if_exists(op.jmp.name, label_vm_pc_map, vm_pc, section); }
    void PhaseScriptVM::op_goto_if(const PSOp& op, PSContext& ctx, const std::unordered_map<std::string, size_t>& label_vm_pc_map, size_t& vm_pc, std::string& section) const {
        uint32_t lv = 0; ctx.get(op.jcc.key, lv);
        if (compare_u32(lv, op.jcc.cmp, op.jcc.imm)) jump_to_label_if_exists(op.jcc.name, label_vm_pc_map, vm_pc, section);
    }
    void PhaseScriptVM::op_goto_if_keys(const PSOp& op, PSContext& ctx, const std::unordered_map<std::string, size_t>& label_vm_pc_map, size_t& vm_pc, std::string& section) const {
        uint32_t lv = 0, rv = 0; ctx.get(op.jcc2.left, lv); ctx.get(op.jcc2.right, rv);
        if (compare_u32(lv, op.jcc2.cmp, rv)) jump_to_label_if_exists(op.jcc2.name, label_vm_pc_map, vm_pc, section);
    }
    void PhaseScriptVM::op_set_u32(const PSOp& op, PSContext& ctx) const { ctx[op.keyimm.key] = op.keyimm.imm; }
    void PhaseScriptVM::op_add_u32(const PSOp& op, PSContext& ctx) const { uint32_t v = 0; ctx.get<uint32_t>(op.keyimm.key, v); ctx[op.keyimm.key] = v + op.keyimm.imm; }
    void PhaseScriptVM::op_step_frames(const PSOp& op) { SCLOGD("[VM] phase=run_inputs begin frames=%zu", op.step.n); if (op.imm.v == 1) host_.setEnableAllBreakpoints(false); for (uint32_t i = 0; i < op.step.n; ++i) host_.stepOneFrameBlocking(); if (op.imm.v == 1) restore_canonical_breakpoint_scope(); SCLOGD("[VM] phase=run_inputs end"); }
    void PhaseScriptVM::op_step_opcode(const PSOp& op) { SCLOGD("[VM] phase=run_inputs begin opcode"); if (op.imm.v == 1) host_.setEnableAllBreakpoints(false); host_.stepOneOpcodeBlocking(); if (op.imm.v == 1) restore_canonical_breakpoint_scope(); SCLOGD("[VM] phase=run_inputs end opcode"); }
    void PhaseScriptVM::op_start_deterministic_run() const { if (!host_.startMovieRecording()) SCLOGE("[VM] Unable to start recording for deterministic run"); }
    void PhaseScriptVM::op_end_deterministic_run() const { host_.endMovieRecording(); }
    bool PhaseScriptVM::op_read_u8(const PSOp& op, PSResult&, PSContext& ctx) { uint8_t v{}; if (!read_u8(op.rd.addr, v)) return false; ctx[op.rd.dst] = v; return true; }
    bool PhaseScriptVM::op_read_u16(const PSOp& op, PSResult&, PSContext& ctx) { uint16_t v{}; if (!read_u16(op.rd.addr, v)) return false; ctx[op.rd.dst] = v; return true; }
    bool PhaseScriptVM::op_read_u32(const PSOp& op, PSResult&, PSContext& ctx) { uint32_t v{}; if (!read_u32(op.rd.addr, v)) { SCLOGD("[VM] READ_U32 FAIL @%08X key=%s", op.rd.addr, savor::context::key::name_for_id(op.rd.dst).data()); return false; } SCLOGD("[VM] READ_U32 @%08X -> %08X key=%s", op.rd.addr, v, savor::context::key::name_for_id(op.rd.dst).data()); ctx[op.rd.dst] = v; return true; }
    bool PhaseScriptVM::op_write_u32(const PSOp& op, PSResult&, PSContext& ctx) {
        uint32_t value = 0;
        uint32_t readback = 0;
        ctx[savor::context::key::core::MEMWRITE_ADDR] = op.rd.addr;
        ctx[savor::context::key::core::MEMWRITE_VALUE] = 0u;
        ctx[savor::context::key::core::MEMWRITE_READBACK] = 0u;
        ctx[savor::context::key::core::MEMWRITE_STATUS] = 0u;

        if (!ctx.get<uint32_t>(op.rd.dst, value)) {
            ctx[savor::context::key::core::MEMWRITE_STATUS] = 2u;
            return true;
        }
        ctx[savor::context::key::core::MEMWRITE_VALUE] = value;

        if (!host_.writeU32(op.rd.addr, value)) {
            ctx[savor::context::key::core::MEMWRITE_STATUS] = 3u;
            return true;
        }
        if (!host_.readU32(op.rd.addr, readback)) {
            ctx[savor::context::key::core::MEMWRITE_STATUS] = 4u;
            return true;
        }
        ctx[savor::context::key::core::MEMWRITE_READBACK] = readback;
        ctx[savor::context::key::core::MEMWRITE_STATUS] = readback == value ? 1u : 5u;
        return true;
    }
    bool PhaseScriptVM::op_read_f32(const PSOp& op, PSResult&, PSContext& ctx) { float v{}; if (!read_f32(op.rd.addr, v)) return false; ctx[op.rd.dst] = v; return true; }
    bool PhaseScriptVM::op_read_f64(const PSOp& op, PSResult&, PSContext& ctx) { double v{}; if (!read_f64(op.rd.addr, v)) return false; ctx[op.rd.dst] = v; return true; }
    void PhaseScriptVM::op_emit_result(const PSOp& op, PSResult& result, PSContext& ctx) const { SCLOGD("[VM] EMIT_RESULT %s=%08X", savor::context::key::name_for_id(op.key.id).data(), ctx[op.key.id]); result.ctx[op.key.id] = ctx[op.key.id]; }
    bool PhaseScriptVM::op_return_result(const PSOp& op, PSResult& result, PSContext& ctx) const { ctx[savor::context::key::core::VI_LAST] = (uint32_t)(host_.getViFieldCountApprox() & 0xFFFFFFFFull); result.ctx = ctx; result.ctx[op.keyimm.key] = op.keyimm.imm; uint32_t dw_outcome = 0; ctx.get(savor::context::key::core::DW_RUN_OUTCOME_CODE, dw_outcome); result.ok = dw_outcome == 0; return true; }
    bool PhaseScriptVM::op_apply_input_from(const PSOp& op, PSResult&, PSContext& ctx) { auto it = ctx.find(op.key.id); if (it == ctx.end()) return false; if (auto p = std::get_if<GCInputFrame>(&it->second)) { host_.setInput(*p); return true; } return false; }
    void PhaseScriptVM::op_set_timeout(const PSOp& op, PSContext& ctx) const { ctx[savor::context::key::core::RUN_MS] = op.imm.v; }
    void PhaseScriptVM::op_set_timeout_from(const PSOp& op, PSContext& ctx) const { uint32_t timeout_ms; ctx.get<uint32_t>(op.key.id, timeout_ms); ctx[savor::context::key::core::RUN_MS] = timeout_ms; }
    bool PhaseScriptVM::op_movie_play_from(const PSOp& op, PSResult&, PSContext& ctx) {
        std::string path;
        ctx.get<std::string>(op.key.id, path);

        host_.clearAllPcBreakpoints();
        armed_ = false;
        armed_pcs_.clear();

        SCLOGI("[VM] MOVIE_PLAY arm-before-start path=%s", path.c_str());
        arm_bps_once();
        if (capture_ && capture_->active()) {
            arm_capture_breakpoints();
        }

        if (!host_.startMoviePlayback(path))
            return false;

        SCLOGI("[VM] MOVIE_PLAY arm-after-boot path=%s movie=%d input=%llu",
            path.c_str(),
            host_.isMoviePlaying() ? 1 : 0,
            static_cast<unsigned long long>(host_.getCurrentMovieInputCount()));
        armed_ = false;
        armed_pcs_.clear();
        arm_bps_once();
        if (capture_ && capture_->active()) {
            arm_capture_breakpoints();
        }
        return true;
    }
    bool PhaseScriptVM::op_save_savestate_from(const PSOp& op, PSResult& result, PSContext& ctx) {
        std::string path;
        ctx.get<std::string>(op.key.id, path);
        if (path.empty()) {
            SCLOGW("[VM] SAVE_SAVESTATE skipped empty path key=%s", savor::context::key::name_for_id(op.key.id).data());
            result.ctx = ctx;
            return true;
        }
        SCLOGI("[VM] SAVE_SAVESTATE begin path=%s", path.c_str());
        if (!host_.saveSavestateBlocking(path)) {
            SCLOGW("[VM] SAVE_SAVESTATE failed path=%s", path.c_str());
            result.ctx = ctx;
            return false;
        }
        ctx[savor::context::key::core::LAST_SAVESTATE_PATH] = path;
        SCLOGI("[VM] SAVE_SAVESTATE end path=%s", path.c_str());
        return true;
    }
    bool PhaseScriptVM::op_require_disc_gameid_from(const PSOp& op, PSResult&, PSContext& ctx) { std::string tmp; ctx.get<std::string>(op.key.id, tmp); if (tmp.size() < 6) return false; auto di = host_.getDiscInfo(); return di.has_value() && di->game_id.size() >= 6 && std::memcmp(di->game_id.data(), tmp.c_str(), 6) == 0; }
    PhaseScriptVM::RunUntilBpCoreResult PhaseScriptVM::run_until_bp_core(PSContext& ctx, const RunUntilBpSpec& spec) {
        using savor::RunToBpOutcome;

        uint32_t timeout_ms = init_.default_timeout_ms;
        uint32_t vi_stall_ms = 0;
        uint32_t progress_flags = 0;
        ctx.get<uint32_t>(savor::context::key::core::RUN_MS, timeout_ms);
        ctx.get<uint32_t>(savor::context::key::core::VI_STALL_MS, vi_stall_ms);
        ctx.get<uint32_t>(savor::context::key::core::PROGRESS_CORE_FLAGS, progress_flags);

        uint32_t poll_ms = spec.poll_ms_override;
        if (poll_ms == 0) {
            ctx.get<uint32_t>(savor::context::key::core::RUN_POLL_MS, poll_ms);
        }
        if (poll_ms == 0 && capture_ && capture_->active()) {
            poll_ms = 1;
        }
        if (poll_ms == 0) {
            poll_ms = host_.pickPollIntervalMs(timeout_ms);
        }

        const auto collect_expected_pcs = [&]() {
            std::vector<uint32_t> pcs;
            pcs.reserve(spec.expected_bp_keys.size() + capture_armed_pcs_.size());
            for (const auto expected_bp : spec.expected_bp_keys) {
                if (const auto* e = bpmap_.find(expected_bp)) {
                    if (std::find(pcs.begin(), pcs.end(), e->pc) == pcs.end()) {
                        pcs.push_back(e->pc);
                    }
                }
            }
            append_capture_pcs(pcs);
            return pcs;
        };

        if (spec.apply_input) {
            host_.setInput(spec.input);
        }

        if (spec.step_off_current_bp) {
            const uint32_t entry_pc = host_.getPC();
            if (const BPAddr* entry_bp = find_hit_bp(
                bpmap_,
                canonical_bp_keys_,
                reserved_bp_keys_,
                predicate_bp_keys_,
                entry_pc)) {
                SCLOGI("[VM] run_until_bp stepoff pc=%08X bp=%u input_btn=%04X",
                    entry_pc,
                    static_cast<uint32_t>(entry_bp->key),
                    spec.input.buttons);
                host_.setEnabledPcBreakpointsOnly({});
                (void)host_.stepOneOpcodeBlocking(static_cast<int>(timeout_ms));
                if (spec.expected_only_scope) {
                    restore_canonical_breakpoint_scope();
                } else {
                    host_.setEnabledPcBreakpointsOnly(collect_expected_pcs());
                }
            }
        }

        if (spec.expected_only_scope) {
            host_.setEnabledPcBreakpointsOnly(collect_expected_pcs());
        }

        const auto t0 = std::chrono::steady_clock::now();
        auto t1 = t0;
        const auto deadline = t0 + std::chrono::milliseconds(timeout_ms);
        DolphinWrapper::RunUntilHitResult rr{};
        uint32_t capture_only_pc_hits = 0;
        uint32_t capture_only_memwatch_hits = 0;
        uint32_t capture_only_unattributed_deltas = 0;
        uint32_t capture_only_last_pc = 0;

        run_until_bp_active_.store(true, std::memory_order_release);
        host_.disableThrottle();
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            const uint32_t remaining_ms = now >= deadline
                ? 1u
                : static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

            rr = host_.runUntilBreakpointFlexible(
                remaining_ms,
                vi_stall_ms,
                spec.watch_movie,
                poll_ms,
                progress_flags);
            t1 = std::chrono::steady_clock::now();

            if (!rr.hit) {
                break;
            }

            const uint32_t hit_pc = static_cast<uint32_t>(rr.pc);
            bool captured_memwatch_event = false;
            bool captured_unattributed_delta = false;
            if (rr.memory_watchpoint.has_value() && capture_ && capture_->active()) {
                const auto& hit = *rr.memory_watchpoint;
                const bool remove_after_capture =
                    capture_->active_memory_watchpoint_is_one_shot(hit.id);
                std::string error;
                if (!capture_->capture_memory_watchpoint_hit(
                    host_,
                    debug_stop_kind_to_string(rr.stop_kind),
                    hit,
                    &error)) {
                    ctx[savor::context::key::core::WORKER_ERROR] = static_cast<uint32_t>(WERR_UnknownError);
                    SCLOGW("[capture] memory watchpoint write failed pc=%08X error=%s", hit_pc, error.c_str());
                    rr = { false, 0u, "capture_failed" };
                    break;
                }
                if (remove_after_capture) {
                    capture_->remove_active_memory_watchpoint(hit.id);
                }
                captured_memwatch_event = true;
            }
            if (!rr.memory_watchpoint.has_value()
                && !rr.memory_watchpoint_deltas.empty()
                && capture_
                && capture_->active()) {
                for (const auto& delta : rr.memory_watchpoint_deltas) {
                    std::string error;
                    if (!capture_->capture_memory_watchpoint_delta(
                        host_,
                        debug_stop_kind_to_string(rr.stop_kind),
                        delta,
                        rr.decoded_current_access,
                        &error)) {
                        ctx[savor::context::key::core::WORKER_ERROR] = static_cast<uint32_t>(WERR_UnknownError);
                        SCLOGW("[capture] memory watchpoint delta write failed pc=%08X error=%s", hit_pc, error.c_str());
                        rr = { false, 0u, "capture_failed" };
                        break;
                    }
                }
                if (!rr.hit) {
                    break;
                }
                captured_memwatch_event = true;
                captured_unattributed_delta = true;
            }
            const bool capture_hit = capture_ && capture_->active() && capture_->contains_pc(hit_pc);
            const BPAddr* program_hit = nullptr;
            if (!spec.expected_bp_keys.empty()) {
                program_hit = find_hit_bp_in_keys(bpmap_, spec.expected_bp_keys, hit_pc);
            }
            if (program_hit == nullptr) {
                program_hit = spec.include_reserved_hit_lookup
                    ? find_hit_bp(bpmap_, canonical_bp_keys_, reserved_bp_keys_, predicate_bp_keys_, hit_pc)
                    : find_hit_bp(bpmap_, canonical_bp_keys_, predicate_bp_keys_, hit_pc);
            }

            if (capture_hit && !capture_current_hit(hit_pc, ctx, spec.capture_watchpoint_scope)) {
                rr = { false, 0u, "capture_failed" };
                break;
            }

            if (program_hit != nullptr) {
                break;
            }

            if (!capture_hit && !captured_memwatch_event) {
                break;
            }

            const uint32_t capture_only_total =
                capture_only_pc_hits + capture_only_memwatch_hits + 1u;
            if (capture_only_total > spec.capture_only_hit_limit) {
                SCLOGW("[capture] capture-only hit limit exceeded limit=%u pc=%08X",
                    spec.capture_only_hit_limit,
                    hit_pc);
                rr = { false, 0u, "capture_hit_limit" };
                break;
            }

            capture_only_last_pc = hit_pc;
            if (capture_hit) {
                ++capture_only_pc_hits;
            }
            if (captured_memwatch_event) {
                ++capture_only_memwatch_hits;
            }
            if (captured_unattributed_delta) {
                capture_only_unattributed_deltas +=
                    static_cast<uint32_t>(rr.memory_watchpoint_deltas.size());
            }

            if (captured_memwatch_event) {
                SCLOGT("[capture] continuing past capture-only memory watchpoint pc=%08X", hit_pc);
                if (!step_past_capture_only_memory_watchpoint(remaining_ms, spec)) {
                    rr = { false, 0u, "capture_failed" };
                    break;
                }
            } else {
                SCLOGT("[capture] continuing past capture-only breakpoint pc=%08X", hit_pc);
                step_past_capture_only_breakpoint(remaining_ms, spec);
            }
        }
        host_.enableThrottle();
        run_until_bp_active_.store(false, std::memory_order_release);

        if (rr.hit && spec.hold_input_through_hit_opcode) {
            SCLOGI("[VM] run_until_bp hold-through-hit pc=%08X input_btn=%04X",
                static_cast<uint32_t>(rr.pc),
                spec.input.buttons);
            host_.setEnabledPcBreakpointsOnly({});
            (void)host_.stepOneOpcodeBlocking(static_cast<int>(timeout_ms));
            if (spec.expected_only_scope) {
                restore_canonical_breakpoint_scope();
            }
        }

        if (spec.release_input) {
            GCInputFrame released_input = spec.input;
            released_input.buttons = static_cast<uint16_t>(released_input.buttons & ~spec.input.buttons);
            host_.setInput(released_input);
        }

        if (spec.expected_only_scope) {
            restore_canonical_breakpoint_scope();
        }

        RunToBpOutcome outcome = RunToBpOutcome::Unknown;
        if (rr.hit) outcome = RunToBpOutcome::Hit;
        else if (rr.reason) {
            if (std::strcmp(rr.reason, "timeout") == 0) outcome = RunToBpOutcome::Timeout;
            else if (std::strcmp(rr.reason, "vi_stalled") == 0) outcome = RunToBpOutcome::ViStalled;
            else if (std::strcmp(rr.reason, "movie_ended") == 0) outcome = RunToBpOutcome::MovieEnded;
            else if (std::strcmp(rr.reason, "capture_failed") == 0) outcome = RunToBpOutcome::Aborted;
            else if (std::strcmp(rr.reason, "capture_hit_limit") == 0) outcome = RunToBpOutcome::Aborted;
        }

        const BPAddr* hit_bp = nullptr;
        uint32_t hit_bp_key = 0;
        bool expected_match = false;
        if (rr.hit) {
            if (!spec.expected_bp_keys.empty()) {
                hit_bp = find_hit_bp_in_keys(bpmap_, spec.expected_bp_keys, static_cast<uint32_t>(rr.pc));
                if (hit_bp != nullptr) {
                    hit_bp_key = static_cast<uint32_t>(hit_bp->key);
                    expected_match = true;
                }
            }
            if (hit_bp == nullptr) {
                hit_bp = spec.include_reserved_hit_lookup
                    ? find_hit_bp(bpmap_, canonical_bp_keys_, reserved_bp_keys_, predicate_bp_keys_, static_cast<uint32_t>(rr.pc))
                    : find_hit_bp(bpmap_, canonical_bp_keys_, predicate_bp_keys_, static_cast<uint32_t>(rr.pc));
                if (hit_bp != nullptr) {
                    hit_bp_key = static_cast<uint32_t>(hit_bp->key);
                }
            }
            if (spec.expected_bp_keys.empty()) {
                expected_match = true;
            }
        }

        const uint32_t elapsed_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
        ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(outcome);
        ctx[savor::context::key::core::ELAPSED_MS] = elapsed_ms;
        ctx[savor::context::key::core::RUN_HIT_PC] = rr.hit ? static_cast<uint32_t>(rr.pc) : 0u;
        ctx[savor::context::key::core::RUN_HIT_BP_KEY] = hit_bp_key;
        ctx[savor::context::key::core::RUN_EXPECTED_MATCH] = expected_match ? 1u : 0u;
        ctx[savor::context::key::core::RUN_STOP_KIND] = static_cast<uint32_t>(rr.stop_kind);
        if (rr.memory_watchpoint.has_value()) {
            const auto& hit = *rr.memory_watchpoint;
            ctx[savor::context::key::core::RUN_MEMWATCH_ID] = hit.id;
            ctx[savor::context::key::core::RUN_MEMWATCH_ADDR] = hit.address;
            ctx[savor::context::key::core::RUN_MEMWATCH_SIZE] = hit.size;
            ctx[savor::context::key::core::RUN_MEMWATCH_ACCESS] = static_cast<uint32_t>(hit.access);
            ctx[savor::context::key::core::RUN_MEMWATCH_HITS_BEFORE] = hit.num_hits_before;
            ctx[savor::context::key::core::RUN_MEMWATCH_HITS_AFTER] = hit.num_hits_after;
        } else {
            ctx[savor::context::key::core::RUN_MEMWATCH_ID] = 0u;
            ctx[savor::context::key::core::RUN_MEMWATCH_ADDR] = 0u;
            ctx[savor::context::key::core::RUN_MEMWATCH_SIZE] = 0u;
            ctx[savor::context::key::core::RUN_MEMWATCH_ACCESS] = 0u;
            ctx[savor::context::key::core::RUN_MEMWATCH_HITS_BEFORE] = 0u;
            ctx[savor::context::key::core::RUN_MEMWATCH_HITS_AFTER] = 0u;
        }
        ctx[savor::context::key::core::VI_DELTA] = static_cast<uint32_t>(host_.getViFieldCountApproxFromBaseline() & 0xFFFFFFFFull);
        ctx[savor::context::key::core::POLL_MS] = poll_ms;
        ctx[savor::context::key::core::VI_LAST] = static_cast<uint32_t>(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_PC_HITS] = capture_only_pc_hits;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_MEMWATCH_HITS] = capture_only_memwatch_hits;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_UNATTRIBUTED_DELTAS] = capture_only_unattributed_deltas;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_LAST_PC] = capture_only_last_pc;

        SCLOGDX(
            SC_TAGS("vm", "breakpoint"),
            "[VM] run_until_bp outcome=%u pc=%08X bp_key=%u bp_symbol=%s expected_match=%u",
            static_cast<uint32_t>(outcome),
            rr.hit ? static_cast<uint32_t>(rr.pc) : 0u,
            hit_bp_key,
            stable_bp_id_or_empty(hit_bp),
            expected_match ? 1u : 0u);

        if (spec.update_derived && derived_) {
            derived_->update_on_bp(hit_bp_key, ctx, host_);
        }

        return RunUntilBpCoreResult{
            .run = rr,
            .outcome = outcome,
            .hit_bp_key = hit_bp_key,
            .expected_match = expected_match,
            .elapsed_ms = elapsed_ms,
        };
    }

    void PhaseScriptVM::op_materialize_battle_macro_steps(PSContext& ctx) {
        using phase::battle::macroprobe::BuildMacroPlanSteps;
        using phase::battle::macroprobe::BuildMacroProbePlanSteps;
        using phase::battle::macroprobe::BuildPlanningContext;
        using phase::battle::macroprobe::DeserializeCommandPlan;
        using phase::battle::macroprobe::FakeAttackMemoryGateMode;
        using phase::battle::macroprobe::FakeAttackPattern;
        using phase::battle::macroprobe::FailureCode;
        using phase::battle::macroprobe::FormatPlanningContext;
        using phase::battle::macroprobe::MacroCommand;
        using phase::battle::macroprobe::MacroMode;
        using phase::battle::macroprobe::MacroStep;

        uint32_t raw_mode = 0;
        uint32_t target_slot = 4;
        uint32_t transition_neutral_frames = 3;
        std::string plan_blob;
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_MODE, raw_mode);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_TARGET_SLOT, target_slot);
        ctx.get<std::string>(savor::context::key::battle::MACRO_PLAN_BLOB, plan_blob);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_TRANSITION_NEUTRAL_FRAMES, transition_neutral_frames);
        end_macro_breakpoint_scope();
        battle_macro_steps_.clear();

        std::vector<MacroCommand> commands;
        FailureCode build_failure = FailureCode::Ok;
        if (!plan_blob.empty()) {
            std::string parse_error;
            if (!DeserializeCommandPlan(plan_blob, &commands, &parse_error)) {
                build_failure = FailureCode::InvalidMode;
                SCLOGW("[battle-macro-probe] invalid plan_blob='%s' error=%s", plan_blob.c_str(), parse_error.c_str());
            }
        } else {
            commands.push_back(MacroCommand{.mode = static_cast<MacroMode>(raw_mode), .target_slot = target_slot});
        }
        ctx[savor::context::key::battle::MACRO_RESULT] = 1u;
        ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(build_failure);
        ctx[savor::context::key::battle::MACRO_STEP_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_LAST_STEP_INDEX] = 0u;
        ctx[savor::context::key::battle::MACRO_LAST_EXPECTED_BP] = 0u;
        ctx[savor::context::key::battle::MACRO_LAST_HIT_BP] = 0u;
        ctx[savor::context::key::battle::MACRO_LAST_HIT_PC] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_PC_HITS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_MEMWATCH_HITS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_UNATTRIBUTED_DELTAS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_LAST_PC] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_ADDR] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_BASELINE] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_LATEST] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_CHANGED] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_POLL_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_ELAPSED_MS] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_GATE_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_BASELINE] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_LATEST] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_CHANGED] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_POLL_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_ELAPSED_MS] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_BASELINE] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_LATEST] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_CHANGED] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_POLL_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_ELAPSED_MS] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_BASELINE] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_LATEST] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_CHANGED] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_POLL_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_ELAPSED_MS] = 0u;
        battle_macro_memory_baseline_valid_ = false;
        battle_macro_memory_addr_ = 0u;
        battle_macro_memory_baseline_ = 0u;

        if (build_failure != FailureCode::Ok) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            SCLOGW("[battle-macro-probe] invalid macro mode=%u target_slot=%u plan='%s' failure=%s",
                raw_mode,
                target_slot,
                plan_blob.c_str(),
                phase::battle::macroprobe::FailureCodeName(build_failure));
            return;
        }

        const uint32_t current_pc = host_.getPC();
        const BPAddr* current_bp = find_hit_bp(bpmap_, canonical_bp_keys_, reserved_bp_keys_, predicate_bp_keys_, current_pc);
        const uint32_t current_bp_key = current_bp != nullptr ? static_cast<uint32_t>(current_bp->key) : 0u;
        if (current_bp_key != static_cast<uint32_t>(bp::battle::TurnInputs)) {
            ctx[savor::context::key::battle::MACRO_LAST_EXPECTED_BP] = static_cast<uint32_t>(bp::battle::TurnInputs);
            const auto rr = run_until_bp_core(ctx, RunUntilBpSpec{
                .expected_bp_keys = {bp::battle::TurnInputs},
                .input = GCInputFrame{},
                .apply_input = true,
                .release_input = true,
                .step_off_current_bp = true,
                .expected_only_scope = true,
                .watch_movie = false,
                .include_reserved_hit_lookup = true,
                .update_derived = false,
            });
            ctx[savor::context::key::battle::MACRO_LAST_HIT_BP] = rr.hit_bp_key;
            ctx[savor::context::key::battle::MACRO_LAST_HIT_PC] = rr.run.hit ? static_cast<uint32_t>(rr.run.pc) : 0u;
            SCLOGI("[battle-macro-probe-preflight] current_pc=%08X current_bp=%u expected=%u hit=%u hit_pc=%08X ok=%d",
                current_pc,
                current_bp_key,
                static_cast<uint32_t>(bp::battle::TurnInputs),
                rr.hit_bp_key,
                rr.run.hit ? static_cast<uint32_t>(rr.run.pc) : 0u,
                rr.expected_match ? 1 : 0);
            if (!rr.run.hit) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::Timeout);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Timeout);
                return;
            }
            if (!rr.expected_match) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::UnexpectedBreakpoint);
                return;
            }
        }

        ctx[savor::context::key::battle::MACRO_LAST_EXPECTED_BP] = static_cast<uint32_t>(bp::battle::BattleMacroInputReadyGate);
        const auto ready_rr = run_until_bp_core(ctx, RunUntilBpSpec{
            .expected_bp_keys = {bp::battle::BattleMacroInputReadyGate},
            .input = GCInputFrame{},
            .apply_input = true,
            .release_input = true,
            .step_off_current_bp = true,
            .expected_only_scope = true,
            .watch_movie = false,
            .include_reserved_hit_lookup = true,
            .update_derived = false,
        });
        ctx[savor::context::key::battle::MACRO_LAST_HIT_BP] = ready_rr.hit_bp_key;
        ctx[savor::context::key::battle::MACRO_LAST_HIT_PC] = ready_rr.run.hit ? static_cast<uint32_t>(ready_rr.run.pc) : 0u;
        SCLOGI("[battle-macro-probe-start-gate] expected=%u hit=%u hit_pc=%08X ok=%d",
            static_cast<uint32_t>(bp::battle::BattleMacroInputReadyGate),
            ready_rr.hit_bp_key,
            ready_rr.run.hit ? static_cast<uint32_t>(ready_rr.run.pc) : 0u,
            ready_rr.expected_match ? 1 : 0);
        if (!ready_rr.run.hit) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::Timeout);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Timeout);
            return;
        }
        if (!ready_rr.expected_match) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::UnexpectedBreakpoint);
            return;
        }

        std::string mem1;
        soa::battle::ctx::BattleContext battle_context{};
        if (!host_.getMem1(mem1)) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::BattleContextUnavailable);
            SCLOGW("[battle-macro-probe] failed to capture MEM1 for planning context");
            return;
        }
        savor::MemView view(reinterpret_cast<const uint8_t*>(mem1.data()), mem1.size());
        if (!soa::battle::ctx::codec::extract_from_mem1(view, battle_context)) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::BattleContextUnavailable);
            SCLOGW("[battle-macro-probe] failed to extract battle planning context from MEM1");
            return;
        }

        const auto planning_context = BuildPlanningContext(battle_context);
        SCLOGI("[battle-macro-probe-planning-context] %s", FormatPlanningContext(planning_context).c_str());

        uint32_t fake_attack_count = 0;
        ctx.get<uint32_t>(savor::context::key::battle::FAKE_ATTACK_COUNT_THIS_TURN, fake_attack_count);
        uint32_t raw_fake_memory_gate_mode = static_cast<uint32_t>(FakeAttackMemoryGateMode::TargetSide);
        uint32_t fake_target_neutral_frames = 0;
        uint32_t fake_input_neutral_frames = 20;
        uint32_t fake_memory_timeout_ms = 1000;
        uint32_t use_mixed_fake_attack_patterns = 0;
        uint32_t raw_first_fake_memory_gate_mode = static_cast<uint32_t>(FakeAttackMemoryGateMode::TargetSide);
        uint32_t first_fake_target_neutral_frames = 0;
        uint32_t first_fake_input_neutral_frames = 20;
        uint32_t first_fake_memory_timeout_ms = 1000;
        uint32_t use_final_fake_attack_pattern = 0;
        uint32_t raw_final_fake_memory_gate_mode = static_cast<uint32_t>(FakeAttackMemoryGateMode::TargetSide);
        uint32_t final_fake_target_neutral_frames = 0;
        uint32_t final_fake_input_neutral_frames = 20;
        uint32_t final_fake_memory_timeout_ms = 1000;
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_MEMORY_GATE_MODE, raw_fake_memory_gate_mode);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_TARGET_NEUTRAL_FRAMES, fake_target_neutral_frames);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_INPUT_NEUTRAL_FRAMES, fake_input_neutral_frames);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_MEMORY_TIMEOUT_MS, fake_memory_timeout_ms);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_USE_MIXED_PATTERNS, use_mixed_fake_attack_patterns);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_FIRST_MEMORY_GATE_MODE, raw_first_fake_memory_gate_mode);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_FIRST_TARGET_NEUTRAL_FRAMES, first_fake_target_neutral_frames);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_FIRST_INPUT_NEUTRAL_FRAMES, first_fake_input_neutral_frames);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_FIRST_MEMORY_TIMEOUT_MS, first_fake_memory_timeout_ms);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_USE_FINAL_PATTERN, use_final_fake_attack_pattern);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_FINAL_MEMORY_GATE_MODE, raw_final_fake_memory_gate_mode);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_FINAL_TARGET_NEUTRAL_FRAMES, final_fake_target_neutral_frames);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_FINAL_INPUT_NEUTRAL_FRAMES, final_fake_input_neutral_frames);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_FINAL_MEMORY_TIMEOUT_MS, final_fake_memory_timeout_ms);
        FakeAttackPattern fake_attack_pattern{
            .memory_gate_mode = static_cast<FakeAttackMemoryGateMode>(raw_fake_memory_gate_mode),
            .target_neutral_before_b_frames = fake_target_neutral_frames,
            .input_neutral_after_b_frames = fake_input_neutral_frames,
            .memory_timeout_ms = fake_memory_timeout_ms,
        };
        FakeAttackPattern first_fake_attack_pattern{
            .memory_gate_mode = static_cast<FakeAttackMemoryGateMode>(raw_first_fake_memory_gate_mode),
            .target_neutral_before_b_frames = first_fake_target_neutral_frames,
            .input_neutral_after_b_frames = first_fake_input_neutral_frames,
            .memory_timeout_ms = first_fake_memory_timeout_ms,
        };
        FakeAttackPattern final_fake_attack_pattern{
            .memory_gate_mode = static_cast<FakeAttackMemoryGateMode>(raw_final_fake_memory_gate_mode),
            .target_neutral_before_b_frames = final_fake_target_neutral_frames,
            .input_neutral_after_b_frames = final_fake_input_neutral_frames,
            .memory_timeout_ms = final_fake_memory_timeout_ms,
        };
        const auto steps = use_final_fake_attack_pattern != 0
            ? BuildMacroProbePlanSteps(
                commands,
                transition_neutral_frames,
                fake_attack_count,
                use_mixed_fake_attack_patterns != 0 ? first_fake_attack_pattern : fake_attack_pattern,
                fake_attack_pattern,
                final_fake_attack_pattern,
                &planning_context,
                &build_failure)
            : use_mixed_fake_attack_patterns != 0
            ? BuildMacroProbePlanSteps(
                commands,
                transition_neutral_frames,
                fake_attack_count,
                first_fake_attack_pattern,
                fake_attack_pattern,
                &planning_context,
                &build_failure)
            : BuildMacroProbePlanSteps(
                commands,
                transition_neutral_frames,
                fake_attack_count,
                fake_attack_pattern,
                &planning_context,
                &build_failure);
        ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(build_failure);
        ctx[savor::context::key::battle::MACRO_STEP_COUNT] = static_cast<uint32_t>(steps.size());
        if (steps.empty() || build_failure != FailureCode::Ok) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            SCLOGW("[battle-macro-probe] invalid live macro plan mode=%u target_slot=%u plan='%s' failure=%s context='%s'",
                raw_mode,
                target_slot,
                plan_blob.c_str(),
                phase::battle::macroprobe::FailureCodeName(build_failure),
                FormatPlanningContext(planning_context).c_str());
            return;
        }

        battle_macro_steps_.reserve(steps.size());
        for (const auto& step : steps) {
            battle_macro_steps_.push_back(RuntimeBreakpointStep{
                .label = step.label ? step.label : "",
                .kind = [kind = step.kind]() {
                    switch (kind) {
                    case MacroStep::Kind::NeutralFrames: return RuntimeMacroStepKind::NeutralFrames;
                    case MacroStep::Kind::CaptureMemoryU32: return RuntimeMacroStepKind::CaptureMemoryU32;
                    case MacroStep::Kind::WaitMemoryU32Changed: return RuntimeMacroStepKind::WaitMemoryU32Changed;
                    case MacroStep::Kind::InputGate:
                    default: return RuntimeMacroStepKind::InputGate;
                    }
                }(),
                .input = step.input,
                .frame_count = step.frame_count,
                .hold_input_through_hit_opcode = step.hold_input_through_hit_opcode,
                .memory_addr = step.memory_addr,
                .memory_timeout_ms = step.memory_timeout_ms,
                .memory_cycle_index = step.memory_cycle_index,
                .expected_bp_keys = step.expected_bps,
            });
        }
        begin_macro_breakpoint_scope();
    }

    void PhaseScriptVM::op_materialize_battle_turn_macro_steps(PSContext& ctx) {
        using phase::battle::macroprobe::BuildMacroPlanStepsFromTurnPlan;
        using phase::battle::macroprobe::BuildPlanningContext;
        using phase::battle::macroprobe::FailureCode;
        using phase::battle::macroprobe::FormatPlanningContext;
        using soa::battle::actions::MaterializeErr;

        end_macro_breakpoint_scope();
        battle_macro_steps_.clear();
        ctx[savor::context::key::battle::MACRO_RESULT] = 1u;
        ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Ok);
        ctx[savor::context::key::battle::MACRO_STEP_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_LAST_STEP_INDEX] = 0u;
        ctx[savor::context::key::battle::MACRO_LAST_EXPECTED_BP] = 0u;
        ctx[savor::context::key::battle::MACRO_LAST_HIT_BP] = 0u;
        ctx[savor::context::key::battle::MACRO_LAST_HIT_PC] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_PC_HITS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_MEMWATCH_HITS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_UNATTRIBUTED_DELTAS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_LAST_PC] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_ADDR] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_BASELINE] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_LATEST] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_CHANGED] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_POLL_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_ELAPSED_MS] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_GATE_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_BASELINE] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_LATEST] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_CHANGED] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_POLL_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_ELAPSED_MS] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_BASELINE] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_LATEST] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_CHANGED] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_POLL_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_ELAPSED_MS] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_BASELINE] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_LATEST] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_CHANGED] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_POLL_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_ELAPSED_MS] = 0u;
        ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = static_cast<uint32_t>(MaterializeErr::OK);
        battle_macro_memory_baseline_valid_ = false;
        battle_macro_memory_addr_ = 0u;
        battle_macro_memory_baseline_ = 0u;

        uint32_t turn = 0;
        ctx.get<uint32_t>(savor::context::key::battle::ACTIVE_TURN, turn);
        if (turn == 0) {
            ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = static_cast<uint32_t>(MaterializeErr::InvalidTurnIdxZero);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::InvalidMode);
            return;
        }

        soa::battle::actions::BattlePath path;
        if (!ctx.get<soa::battle::actions::BattlePath>(savor::context::key::battle::TURN_PLANS, path)) {
            ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = static_cast<uint32_t>(MaterializeErr::BadBlob);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::InvalidMode);
            return;
        }
        if (turn > path.size()) {
            ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = static_cast<uint32_t>(MaterializeErr::OutOfTurns);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::InvalidMode);
            return;
        }

        const uint32_t current_pc = host_.getPC();
        const BPAddr* current_bp = find_hit_bp(bpmap_, canonical_bp_keys_, reserved_bp_keys_, predicate_bp_keys_, current_pc);
        const uint32_t current_bp_key = current_bp != nullptr ? static_cast<uint32_t>(current_bp->key) : 0u;
        if (current_bp_key != static_cast<uint32_t>(bp::battle::TurnInputs)) {
            ctx[savor::context::key::battle::MACRO_LAST_EXPECTED_BP] = static_cast<uint32_t>(bp::battle::TurnInputs);
            const auto rr = run_until_bp_core(ctx, RunUntilBpSpec{
                .expected_bp_keys = {bp::battle::TurnInputs},
                .input = GCInputFrame{},
                .apply_input = true,
                .release_input = true,
                .step_off_current_bp = true,
                .expected_only_scope = true,
                .watch_movie = false,
                .include_reserved_hit_lookup = true,
                .update_derived = false,
            });
            ctx[savor::context::key::battle::MACRO_LAST_HIT_BP] = rr.hit_bp_key;
            ctx[savor::context::key::battle::MACRO_LAST_HIT_PC] = rr.run.hit ? static_cast<uint32_t>(rr.run.pc) : 0u;
            SCLOGI("[battle-turn-macro-preflight] current_pc=%08X current_bp=%u expected=%u hit=%u hit_pc=%08X ok=%d",
                current_pc,
                current_bp_key,
                static_cast<uint32_t>(bp::battle::TurnInputs),
                rr.hit_bp_key,
                rr.run.hit ? static_cast<uint32_t>(rr.run.pc) : 0u,
                rr.expected_match ? 1 : 0);
            if (!rr.run.hit) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::Timeout);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Timeout);
                return;
            }
            if (!rr.expected_match) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::UnexpectedBreakpoint);
                return;
            }
        }

        ctx[savor::context::key::battle::MACRO_LAST_EXPECTED_BP] = static_cast<uint32_t>(bp::battle::BattleMacroInputReadyGate);
        const auto ready_rr = run_until_bp_core(ctx, RunUntilBpSpec{
            .expected_bp_keys = {bp::battle::BattleMacroInputReadyGate},
            .input = GCInputFrame{},
            .apply_input = true,
            .release_input = true,
            .step_off_current_bp = true,
            .expected_only_scope = true,
            .watch_movie = false,
            .include_reserved_hit_lookup = true,
            .update_derived = false,
        });
        ctx[savor::context::key::battle::MACRO_LAST_HIT_BP] = ready_rr.hit_bp_key;
        ctx[savor::context::key::battle::MACRO_LAST_HIT_PC] = ready_rr.run.hit ? static_cast<uint32_t>(ready_rr.run.pc) : 0u;
        SCLOGI("[battle-turn-macro-start-gate] expected=%u hit=%u hit_pc=%08X ok=%d",
            static_cast<uint32_t>(bp::battle::BattleMacroInputReadyGate),
            ready_rr.hit_bp_key,
            ready_rr.run.hit ? static_cast<uint32_t>(ready_rr.run.pc) : 0u,
            ready_rr.expected_match ? 1 : 0);
        if (!ready_rr.run.hit) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::Timeout);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Timeout);
            return;
        }
        if (!ready_rr.expected_match) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::UnexpectedBreakpoint);
            return;
        }

        std::string mem1;
        soa::battle::ctx::BattleContext battle_context{};
        if (!host_.getMem1(mem1)) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::BattleContextUnavailable);
            SCLOGW("[battle-turn-macro] failed to capture MEM1 for planning context");
            return;
        }
        savor::MemView view(reinterpret_cast<const uint8_t*>(mem1.data()), mem1.size());
        if (!soa::battle::ctx::codec::extract_from_mem1(view, battle_context)) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::BattleContextUnavailable);
            SCLOGW("[battle-turn-macro] failed to extract battle planning context from MEM1");
            return;
        }

        const auto planning_context = BuildPlanningContext(battle_context);
        SCLOGI("[battle-turn-macro-planning-context] %s", FormatPlanningContext(planning_context).c_str());

        uint32_t transition_neutral_frames = 3;
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_TRANSITION_NEUTRAL_FRAMES, transition_neutral_frames);
        MaterializeErr materialize_err = MaterializeErr::OK;
        const auto steps = BuildMacroPlanStepsFromTurnPlan(
            path[turn - 1],
            transition_neutral_frames,
            &planning_context,
            &materialize_err);

        ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = static_cast<uint32_t>(materialize_err);
        ctx[savor::context::key::battle::MACRO_STEP_COUNT] = static_cast<uint32_t>(steps.size());
        if (materialize_err != MaterializeErr::OK || steps.empty()) {
            const auto failure = materialize_err == MaterializeErr::NoValidTarget
                ? FailureCode::InvalidTarget
                : FailureCode::InvalidMode;
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(failure);
            SCLOGW("[battle-turn-macro] failed to materialize turn=%u failure=%s macro_failure=%s context='%s'",
                turn,
                soa::battle::actions::get_materialize_err_string(materialize_err).c_str(),
                phase::battle::macroprobe::FailureCodeName(failure),
                FormatPlanningContext(planning_context).c_str());
            return;
        }

        battle_macro_steps_.reserve(steps.size());
        for (const auto& step : steps) {
            battle_macro_steps_.push_back(RuntimeBreakpointStep{
                .label = step.label ? step.label : "",
                .kind = [kind = step.kind]() {
                    switch (kind) {
                    case phase::battle::macroprobe::MacroStep::Kind::NeutralFrames: return RuntimeMacroStepKind::NeutralFrames;
                    case phase::battle::macroprobe::MacroStep::Kind::CaptureMemoryU32: return RuntimeMacroStepKind::CaptureMemoryU32;
                    case phase::battle::macroprobe::MacroStep::Kind::WaitMemoryU32Changed: return RuntimeMacroStepKind::WaitMemoryU32Changed;
                    case phase::battle::macroprobe::MacroStep::Kind::InputGate:
                    default: return RuntimeMacroStepKind::InputGate;
                    }
                }(),
                .input = step.input,
                .frame_count = step.frame_count,
                .hold_input_through_hit_opcode = step.hold_input_through_hit_opcode,
                .memory_addr = step.memory_addr,
                .memory_timeout_ms = step.memory_timeout_ms,
                .memory_cycle_index = step.memory_cycle_index,
                .expected_bp_keys = step.expected_bps,
            });
        }
        begin_macro_breakpoint_scope();
        ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Ok);
    }

    void PhaseScriptVM::op_execute_battle_macro_step(PSContext& ctx) {
        using phase::battle::macroprobe::FailureCode;

        uint32_t step_index = 0;
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_LAST_STEP_INDEX, step_index);
        if (step_index >= battle_macro_steps_.size()) {
            end_macro_breakpoint_scope();
            ctx[savor::context::key::battle::MACRO_RESULT] = 0u;
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Ok);
            return;
        }

        const auto& step = battle_macro_steps_[step_index];
        const uint32_t expected_first = step.expected_bp_keys.empty() ? 0u : static_cast<uint32_t>(step.expected_bp_keys.front());
        ctx[savor::context::key::battle::MACRO_LAST_STEP_INDEX] = step_index;
        ctx[savor::context::key::battle::MACRO_LAST_EXPECTED_BP] = expected_first;
        ctx[savor::context::key::battle::MACRO_LAST_HIT_BP] = 0u;
        ctx[savor::context::key::battle::MACRO_LAST_HIT_PC] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_PC_HITS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_MEMWATCH_HITS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_UNATTRIBUTED_DELTAS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_LAST_PC] = 0u;

        const auto advance_macro_step = [&]() {
            if (step_index + 1u >= battle_macro_steps_.size()) {
                end_macro_breakpoint_scope();
                ctx[savor::context::key::battle::MACRO_RESULT] = 0u;
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Ok);
            } else {
                ctx[savor::context::key::battle::MACRO_LAST_STEP_INDEX] = step_index + 1u;
            }
        };

        if (step.kind == RuntimeMacroStepKind::CaptureMemoryU32) {
            clear_capture_memory_watchpoints();
            uint32_t value = 0;
            if (!read_u32(step.memory_addr, value)) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::MemoryReadFailed);
                end_macro_breakpoint_scope();
                SCLOGW("[battle-macro-memory-gate] label=%s capture_failed=true addr=%08X",
                    step.label.c_str(),
                    step.memory_addr);
                return;
            }
            battle_macro_memory_baseline_valid_ = true;
            battle_macro_memory_addr_ = step.memory_addr;
            battle_macro_memory_baseline_ = value;
            ctx[savor::context::key::battle::MACRO_MEMORY_ADDR] = step.memory_addr;
            ctx[savor::context::key::battle::MACRO_MEMORY_BASELINE] = value;
            ctx[savor::context::key::battle::MACRO_MEMORY_LATEST] = value;
            ctx[savor::context::key::battle::MACRO_MEMORY_CHANGED] = 0u;
            ctx[savor::context::key::battle::MACRO_MEMORY_POLL_COUNT] = 0u;
            ctx[savor::context::key::battle::MACRO_MEMORY_ELAPSED_MS] = 0u;
            SCLOGI("[battle-macro-memory-gate] label=%s capture=true addr=%08X before=%08X",
                step.label.c_str(),
                step.memory_addr,
                value);
            advance_macro_step();
            return;
        }

        if (step.kind == RuntimeMacroStepKind::WaitMemoryU32Changed) {
            clear_capture_memory_watchpoints();
            if (!battle_macro_memory_baseline_valid_ || battle_macro_memory_addr_ != step.memory_addr) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::MemoryReadFailed);
                end_macro_breakpoint_scope();
                SCLOGW("[battle-macro-memory-gate] label=%s missing_baseline=true addr=%08X baseline_addr=%08X",
                    step.label.c_str(),
                    step.memory_addr,
                    battle_macro_memory_addr_);
                return;
            }

            host_.setInput(GCInputFrame{});
            host_.setEnabledPcBreakpointsOnly({});
            const auto start = std::chrono::steady_clock::now();
            const uint32_t timeout_ms = step.memory_timeout_ms != 0 ? step.memory_timeout_ms : 1000u;
            uint32_t latest = battle_macro_memory_baseline_;
            uint32_t polls = 0;
            bool changed = false;
            bool read_failed = false;
            for (;;) {
                if (!read_u32(step.memory_addr, latest)) {
                    read_failed = true;
                    break;
                }
                if (latest != battle_macro_memory_baseline_) {
                    changed = true;
                    break;
                }
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (elapsed >= timeout_ms) {
                    break;
                }
                host_.stepOneFrameBlocking();
                ++polls;
            }
            if (!macro_breakpoint_scope_active_) {
                restore_canonical_breakpoint_scope();
            }
            const uint32_t elapsed_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count());
            ctx[savor::context::key::battle::MACRO_MEMORY_ADDR] = step.memory_addr;
            ctx[savor::context::key::battle::MACRO_MEMORY_BASELINE] = battle_macro_memory_baseline_;
            ctx[savor::context::key::battle::MACRO_MEMORY_LATEST] = latest;
            ctx[savor::context::key::battle::MACRO_MEMORY_CHANGED] = changed ? 1u : 0u;
            ctx[savor::context::key::battle::MACRO_MEMORY_POLL_COUNT] = polls;
            ctx[savor::context::key::battle::MACRO_MEMORY_ELAPSED_MS] = elapsed_ms;
            uint32_t gate_count = 0;
            ctx.get<uint32_t>(savor::context::key::battle::MACRO_MEMORY_GATE_COUNT, gate_count);
            ctx[savor::context::key::battle::MACRO_MEMORY_GATE_COUNT] = gate_count + 1u;
            if (step.memory_cycle_index == 0) {
                ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_BASELINE] = battle_macro_memory_baseline_;
                ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_LATEST] = latest;
                ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_CHANGED] = changed ? 1u : 0u;
                ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_POLL_COUNT] = polls;
                ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_ELAPSED_MS] = elapsed_ms;
            } else if (step.memory_cycle_index == 1) {
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_BASELINE] = battle_macro_memory_baseline_;
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_LATEST] = latest;
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_CHANGED] = changed ? 1u : 0u;
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_POLL_COUNT] = polls;
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_ELAPSED_MS] = elapsed_ms;
            } else if (step.memory_cycle_index == 2) {
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_BASELINE] = battle_macro_memory_baseline_;
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_LATEST] = latest;
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_CHANGED] = changed ? 1u : 0u;
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_POLL_COUNT] = polls;
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_ELAPSED_MS] = elapsed_ms;
            }
            SCLOGI("[battle-macro-memory-gate] label=%s addr=%08X before=%08X after=%08X changed=%u polls=%u elapsed_ms=%u timeout_ms=%u",
                step.label.c_str(),
                step.memory_addr,
                battle_macro_memory_baseline_,
                latest,
                changed ? 1u : 0u,
                polls,
                elapsed_ms,
                timeout_ms);
            if (read_failed) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::MemoryReadFailed);
                end_macro_breakpoint_scope();
                return;
            }
            if (!changed) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::Timeout);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Timeout);
                end_macro_breakpoint_scope();
                return;
            }
            advance_macro_step();
            return;
        }

        if (step.kind == RuntimeMacroStepKind::NeutralFrames) {
            clear_capture_memory_watchpoints();
            host_.setInput(GCInputFrame{});
            host_.setEnabledPcBreakpointsOnly({});
            for (uint32_t frame = 0; frame < step.frame_count; ++frame) {
                host_.stepOneFrameBlocking();
            }
            if (!macro_breakpoint_scope_active_) {
                restore_canonical_breakpoint_scope();
            }
            SCLOGI("[battle-macro-probe-step] index=%u label=%s neutral_frames=%u ok=1",
                step_index,
                step.label.c_str(),
                step.frame_count);
            if (derived_) derived_->update_on_bp(0u, ctx, host_);
            advance_macro_step();
            return;
        }

        if (step.expected_bp_keys.size() != 1u) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::InvalidMode);
            SCLOGW("[battle-macro-probe-step] invalid_expected_count index=%u label=%s expected=%s",
                step_index,
                step.label.c_str(),
                bp_key_list_desc(step.expected_bp_keys).c_str());
            end_macro_breakpoint_scope();
            return;
        }

        if (!arm_capture_memory_watchpoints(savor::capture::WatchpointScope::InputMacro)) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] =
                static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] =
                static_cast<uint32_t>(FailureCode::UnexpectedBreakpoint);
            end_macro_breakpoint_scope();
            return;
        }
        enable_macro_step_breakpoint(step.expected_bp_keys.front());
        const auto rr = run_until_bp_core(ctx, RunUntilBpSpec{
            .expected_bp_keys = step.expected_bp_keys,
            .input = step.input,
            .apply_input = true,
            .release_input = false,
            .hold_input_through_hit_opcode = step.hold_input_through_hit_opcode,
            .step_off_current_bp = true,
            .expected_only_scope = true,
            .watch_movie = false,
            .include_reserved_hit_lookup = true,
            .update_derived = false,
            .capture_watchpoint_scope = savor::capture::WatchpointScope::InputMacro,
            .capture_only_hit_limit = 1024u,
        });
        disable_macro_step_breakpoint();
        clear_capture_memory_watchpoints();
        GCInputFrame released_input = step.input;
        released_input.buttons = static_cast<uint16_t>(released_input.buttons & ~step.input.buttons);
        host_.setInput(released_input);
        ctx[savor::context::key::battle::MACRO_LAST_HIT_BP] = rr.hit_bp_key;
        ctx[savor::context::key::battle::MACRO_LAST_HIT_PC] = rr.run.hit ? static_cast<uint32_t>(rr.run.pc) : 0u;

        SCLOGI("[battle-macro-probe-step] index=%u label=%s expected=%s hit=%u hit_pc=%08X ok=%d",
            step_index,
            step.label.c_str(),
            bp_key_list_desc(step.expected_bp_keys).c_str(),
            rr.hit_bp_key,
            rr.run.hit ? static_cast<uint32_t>(rr.run.pc) : 0u,
            rr.expected_match ? 1 : 0);

        if (!rr.run.hit) {
            if (rr.outcome == RunToBpOutcome::Aborted) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::Aborted);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] =
                    static_cast<uint32_t>(FailureCode::CaptureOnlyHitLimit);
            } else {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::Timeout);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Timeout);
            }
            end_macro_breakpoint_scope();
            return;
        }
        if (!rr.expected_match) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::UnexpectedBreakpoint);
            end_macro_breakpoint_scope();
            return;
        }

        if (derived_) derived_->update_on_bp(rr.hit_bp_key, ctx, host_);
        advance_macro_step();
    }

    void PhaseScriptVM::op_build_turn_inputplan_from_battle_path(PSContext& ctx) const {
        uint32_t turn = 0;
        ctx.get<uint32_t>(savor::context::key::battle::ACTIVE_TURN, turn);
        if (turn == 0) {
            ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = (uint32_t)soa::battle::actions::MaterializeErr::InvalidTurnIdxZero;
            ctx[savor::context::key::core::PLAN_DONE] = (uint32_t)1;
        }
        soa::battle::actions::BattlePath bp;
        if (!ctx.get<soa::battle::actions::BattlePath>(savor::context::key::battle::TURN_PLANS, bp)) {
            ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = (uint32_t)soa::battle::actions::MaterializeErr::BadBlob;
            ctx[savor::context::key::core::PLAN_DONE] = (uint32_t)1;
            return;
        }
        if (turn > bp.size()) {
            ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = (uint32_t)soa::battle::actions::MaterializeErr::OutOfTurns;
            ctx[savor::context::key::core::PLAN_DONE] = (uint32_t)1;
            return;
        }
        soa::battle::ctx::BattleContext bc{};
        std::string blob;
        if (ctx.get<std::string>(savor::context::key::battle::CTX_BLOB, blob)) soa::battle::ctx::codec::decode(blob, bc);
        const auto& turn_plan = bp[turn - 1];
        savor::ControllerInputSequence plan;
        auto err = soa::battle::actions::MaterializeErr::OK;
        if (!soa::battle::actions::MaterializeBattleTurnInputs(bc, turn_plan, plan, err)) {
            ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = (uint32_t)err;
            ctx[savor::context::key::core::PLAN_DONE] = (uint32_t)1;
            return;
        }
        const uint32_t n = static_cast<uint32_t>(plan.size());
        std::string counts; counts.resize(sizeof(uint32_t)); std::memcpy(counts.data(), &n, sizeof(uint32_t));
        std::string frames; frames.resize(n * sizeof(savor::GCInputFrame)); if (n) std::memcpy(frames.data(), plan.data(), frames.size());
        ctx[savor::context::key::battle::NUM_TURN_PLANS] = uint32_t(1);
        ctx[savor::context::key::battle::INPUTPLAN_FRAME_COUNT] = counts;
        ctx[savor::context::key::battle::INPUTPLAN] = frames;
        ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = uint32_t((uint32_t)soa::battle::actions::MaterializeErr::OK);
        ctx[savor::context::key::core::PLAN_DONE] = uint32_t(0);
    }

    void PhaseScriptVM::op_apply_battle_inputplan_frames(PSContext& ctx) {
        auto itC = ctx.find(savor::context::key::battle::INPUTPLAN_FRAME_COUNT);
        auto itT = ctx.find(savor::context::key::battle::INPUTPLAN);
        ctx[savor::context::key::battle::INPUT_PLAYBACK_ERR] = uint32_t(0);
        ctx[savor::context::key::battle::INPUT_PLAYBACK_UNACKED] = uint32_t(0);
        if (itC == ctx.end() || itT == ctx.end()) {
            ctx[savor::context::key::battle::INPUT_PLAYBACK_ERR] = uint32_t(1);
            return;
        }
        const auto* counts_s = std::get_if<std::string>(&itC->second);
        const auto* table_s = std::get_if<std::string>(&itT->second);
        if (!counts_s || !table_s || counts_s->size() < sizeof(uint32_t)) {
            ctx[savor::context::key::battle::INPUT_PLAYBACK_ERR] = uint32_t(2);
            return;
        }
        const uint8_t* counts = (const uint8_t*)counts_s->data();
        const uint8_t* frames = (const uint8_t*)table_s->data();
        if (counts == 0) { ctx[savor::context::key::core::PLAN_DONE] = uint32_t(1); return; }
        const uint32_t apply_vi_start = static_cast<uint32_t>(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
        host_.setEnableAllBreakpoints(false);
        uint32_t count = 0;
        std::memcpy(&count, counts, sizeof(uint32_t));
        if (table_s->size() < static_cast<size_t>(count) * sizeof(GCInputFrame)) {
            restore_canonical_breakpoint_scope();
            ctx[savor::context::key::battle::INPUT_PLAYBACK_ERR] = uint32_t(2);
            return;
        }
        savor::ControllerInputSequence plan{}; plan.reserve(count);
        uint32_t rand{ 0 };
        host_.readU32(addr::AddrRegistry::base(addr::core::RNG_SEED), rand);
        SCLOGTX(SC_TAGS("vm", "input", "rng"), "RNG before inputs %X", rand);
        for (uint32_t idx = 0; idx < count; idx++) {
            GCInputFrame f{};
            std::memcpy(&f, frames + (idx * sizeof(GCInputFrame)), sizeof(GCInputFrame));
            plan.push_back(f);
        }
        uint32_t retry_count = 0;
        ctx.get(savor::context::key::battle::INPUT_RETRY_COUNT, retry_count);
        std::string playback_label = std::format("battle_turn_attempt_{}", retry_count);
        const auto playback = host_.playInputTapeBlocking(
            plan,
            DolphinWrapper::InputTapePlaybackOptions{
                .max_unacked_replays = 2,
                .safe_mode = retry_count > 0,
                .label = playback_label.c_str(),
            });
        host_.readU32(addr::AddrRegistry::base(addr::core::RNG_SEED), rand);
        SCLOGTX(SC_TAGS("vm", "input", "rng"), "RNG after inputs %X", rand);
        restore_canonical_breakpoint_scope();
        ctx[savor::context::key::battle::INPUT_PLAYBACK_UNACKED] = playback.unacked_count;
        if (!playback.ok) {
            ctx[savor::context::key::battle::INPUT_PLAYBACK_ERR] = uint32_t(3);
            ctx[savor::context::key::core::PLAN_DONE] = uint32_t(0);
            SCLOGDX(SC_TAGS("vm", "input"),
                "[VM] input playback failed attempt=%u failed_index=%u unacked=%u",
                retry_count,
                playback.failed_index,
                playback.unacked_count);
            return;
        }
        const uint32_t apply_vi_end = static_cast<uint32_t>(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
        uint32_t turn_number = 0;
        if (!ctx.get(savor::context::key::battle::TURN_OUTPUT_INDEX, turn_number)) (void)ctx.get(savor::context::key::battle::ACTIVE_TURN, turn_number);
        std::string turn_blob; (void)ctx.get(savor::context::key::battle::APPLIED_INPUTPLAN_TURN_BLOB, turn_blob);
        savor::inputtrace::BattleTurnInputTrace chunk{};
        chunk.turn_number = turn_number;
        chunk.vi_start = apply_vi_start;
        chunk.vi_end = apply_vi_end;
        chunk.frames = playback.attempted_frames;
        chunk.vi_durations = playback.vi_durations;
        (void)savor::inputtrace::append_turn_input_trace(turn_blob, chunk);
        ctx[savor::context::key::battle::APPLIED_INPUTPLAN_TURN_BLOB] = std::move(turn_blob);
        ctx[savor::context::key::battle::APPLIED_INPUTPLAN_COUNT] = static_cast<uint32_t>(playback.attempted_frames.size());
        ctx[savor::context::key::core::PLAN_DONE] = uint32_t(1);
        uint32_t cur_turn_plans = 0;
        ctx.get(savor::context::key::battle::NUM_TURN_PLANS, cur_turn_plans);
        ctx[savor::context::key::battle::NUM_TURN_PLANS] = cur_turn_plans > 0 ? cur_turn_plans - 1 : 0;
    }
    void PhaseScriptVM::op_run_until_bp(PSContext& ctx) {
        (void)run_until_bp_core(ctx, RunUntilBpSpec{});
    }
    void PhaseScriptVM::op_run_until_bp_key(const PSOp& op, PSContext& ctx) {
        (void)run_until_bp_core(ctx, RunUntilBpSpec{
            .expected_bp_keys = { static_cast<BPKey>(op.imm.v) },
            .expected_only_scope = true,
            .include_reserved_hit_lookup = true,
        });
    }
    void PhaseScriptVM::op_run_until_debug_stop(PSContext& ctx) {
        (void)run_until_bp_core(ctx, RunUntilBpSpec{});
    }
    bool PhaseScriptVM::op_arm_memory_watchpoint(const PSOp& op, PSResult& result, PSContext& ctx) {
        uint32_t address = op.memwatch.address;
        if (op.memwatch.use_address_key != 0) {
            if (!ctx.get<uint32_t>(op.memwatch.address_key, address)) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
                ctx[savor::context::key::core::WORKER_ERROR] = static_cast<uint32_t>(WERR_UnknownError);
                result.ctx = ctx;
                return false;
            }
        }

        const auto access = static_cast<DolphinWrapper::MemoryWatchpointAccess>(op.memwatch.access);
        if (access != DolphinWrapper::MemoryWatchpointAccess::Read
            && access != DolphinWrapper::MemoryWatchpointAccess::Write
            && access != DolphinWrapper::MemoryWatchpointAccess::Access) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::core::WORKER_ERROR] = static_cast<uint32_t>(WERR_UnknownError);
            result.ctx = ctx;
            return false;
        }

        const DolphinWrapper::MemoryWatchpointSpec spec{
            .id = op.memwatch.id,
            .address = address,
            .size = op.memwatch.size,
            .access = access,
        };
        if (!host_.armMemoryWatchpoints({ spec })) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::core::WORKER_ERROR] = static_cast<uint32_t>(WERR_UnknownError);
            result.ctx = ctx;
            return false;
        }
        return true;
    }
    void PhaseScriptVM::op_clear_memory_watchpoints() const {
        host_.clearMemoryWatchpoints();
        if (capture_ && capture_->active()) {
            capture_->set_active_memory_watchpoints({});
        }
    }
    bool PhaseScriptVM::op_capture_seed_override(PSResult& result, PSContext& ctx) {
        if (!capture_ || !capture_->active()) {
            return true;
        }

        uint32_t original_seed = 0;
        uint32_t override_seed = 0;
        uint32_t applied_seed = 0;
        ctx.get<uint32_t>(savor::context::key::battle::RNG_ORIGINAL_SEED, original_seed);
        ctx.get<uint32_t>(savor::context::key::battle::RNG_OVERRIDE_SEED, override_seed);
        ctx.get<uint32_t>(savor::context::key::battle::RNG_APPLIED_SEED, applied_seed);

        std::string error;
        if (!capture_->capture_seed_override(host_, host_.getPC(), original_seed, override_seed, applied_seed, &error)) {
            ctx[savor::context::key::core::WORKER_ERROR] = static_cast<uint32_t>(WERR_UnknownError);
            result.ctx = ctx;
            SCLOGW("[capture] seed override write failed error=%s", error.c_str());
            return false;
        }
        return true;
    }
    bool PhaseScriptVM::op_arm_capture_memory_watchpoints(PSResult& result, PSContext& ctx) {
        if (!capture_ || !capture_->active()) {
            return true;
        }
        if (arm_capture_memory_watchpoints(savor::capture::WatchpointScope::Normal)) {
            return true;
        }
        ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
        ctx[savor::context::key::core::WORKER_ERROR] = static_cast<uint32_t>(WERR_UnknownError);
        result.ctx = ctx;
        return false;
    }
    void PhaseScriptVM::op_record_current_bp(PSContext& ctx) {
        const uint32_t pc = host_.getPC();
        const BPAddr* hit_bp = find_hit_bp(bpmap_, canonical_bp_keys_, predicate_bp_keys_, pc);
        uint32_t hit_bp_key = hit_bp != nullptr ? static_cast<uint32_t>(hit_bp->key) : 0u;
        SCLOGDX(
            SC_TAGS("vm", "breakpoint"),
            "[VM] record_current_bp pc=%08X bp_key=%u bp_symbol=%s",
            pc,
            hit_bp_key,
            stable_bp_id_or_empty(hit_bp));
        ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = hit_bp_key != 0
            ? static_cast<uint32_t>(RunToBpOutcome::Hit)
            : static_cast<uint32_t>(RunToBpOutcome::Unknown);
        ctx[savor::context::key::core::RUN_HIT_PC] = pc;
        ctx[savor::context::key::core::RUN_HIT_BP_KEY] = hit_bp_key;
        ctx[savor::context::key::core::RUN_STOP_KIND] = hit_bp_key != 0
            ? static_cast<uint32_t>(DolphinWrapper::DebugStopKind::PcBreakpoint)
            : static_cast<uint32_t>(DolphinWrapper::DebugStopKind::None);
        ctx[savor::context::key::core::VI_DELTA] = 0u;
        ctx[savor::context::key::core::VI_LAST] = static_cast<uint32_t>(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
        if (derived_ && hit_bp_key != 0) derived_->update_on_bp(hit_bp_key, ctx, host_);
    }
    void PhaseScriptVM::op_record_tas_input_sample(PSContext& ctx) {
        uint32_t sample_count = 0;
        ctx.get<uint32_t>(savor::context::key::tasframedetector::SAMPLE_COUNT, sample_count);

        const uint32_t vi = static_cast<uint32_t>(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
        const uint64_t input_count = host_.getCurrentMovieInputCount();
        const uint32_t movie_ended = host_.isMoviePlaybackEnded() ? 1u : 0u;

        std::string stream_ini;
        (void)ctx.get<std::string>(savor::context::key::tasframedetector::STREAM_INI, stream_ini);
        IniDoc doc = stream_ini.empty() ? IniDoc{} : IniDoc::parse(stream_ini);
        doc.ensure_section("FrameSamples");
        const std::string idx = std::to_string(sample_count);
        doc.set("FrameSamples", "frame." + idx, idx);
        doc.set("FrameSamples", "vi." + idx, std::to_string(vi));
        doc.set("FrameSamples", "input_count." + idx, std::to_string(input_count));
        doc.set("FrameSamples", "movie_ended." + idx, std::to_string(movie_ended));
        doc.set("FrameSamples", "count", std::to_string(sample_count + 1));

        ctx[savor::context::key::tasframedetector::STREAM_INI] = doc.to_string_sorted();
        ctx[savor::context::key::tasframedetector::SAMPLE_COUNT] = sample_count + 1;
        ctx[savor::context::key::tasframedetector::MOVIE_ENDED] = movie_ended;
        ctx[savor::context::key::tasframedetector::INPUT_COUNT] = static_cast<uint32_t>(input_count & 0xFFFFFFFFu);

        SCLOGT("[TasInputStreamDetector] sample=%u vi=%u input_count=%llu movie_ended=%u",
            sample_count,
            vi,
            static_cast<unsigned long long>(input_count),
            movie_ended);
    }
    void PhaseScriptVM::op_get_battle_context(PSResult& result, PSContext& ctx) const {
        std::string mem1;
        if (!host_.getMem1(mem1)) { result.ok = false; return; }
        savor::MemView view(reinterpret_cast<const uint8_t*>(mem1.data()), mem1.size());
        soa::battle::ctx::BattleContext bc{};
        if (!soa::battle::ctx::codec::extract_from_mem1(view, bc)) { result.ok = false; return; }
        std::string blob; soa::battle::ctx::codec::encode(bc, blob);
        ctx[savor::context::key::battle::CTX_BLOB] = blob;
    }
    void PhaseScriptVM::op_arm_bps_from_pred_table(PSContext& ctx) {
        auto itN = ctx.find(savor::context::key::core::PRED_COUNT);
        auto itT = ctx.find(savor::context::key::core::PRED_TABLE);
        if (itN == ctx.end() || itT == ctx.end()) return;
        const uint32_t n = std::get<uint32_t>(itN->second);
        const auto* tbl = std::get_if<std::string>(&itT->second);
        if (!n || !tbl) return;
        const auto* rec = reinterpret_cast<const pred::PredicateRecord*>(tbl->data());
        std::vector<uint32_t> bp_keys; bp_keys.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            if (rec[i].required_bp != 0) {
                bp_keys.push_back(rec[i].required_bp);
            }
            append_record_baseline_bps(*tbl, rec[i], bp_keys);
        }
        std::sort(bp_keys.begin(), bp_keys.end());
        bp_keys.erase(std::unique(bp_keys.begin(), bp_keys.end()), bp_keys.end());

        std::vector<uint32_t> pcs; pcs.reserve(bp_keys.size());
        for (const auto bp_key : bp_keys) {
            const BPAddr* e = bpmap_.find(static_cast<BPKey>(bp_key));
            if (!e || !e->pc) continue;
            pcs.push_back(e->pc); predicate_bp_keys_.push_back(e->key);
        }
        if (!pcs.empty()) host_.armPcBreakpoints(pcs);
    }
    void PhaseScriptVM::op_capture_pred_baselines(PSContext& ctx, KeyHostRouter& router) {
        auto itN = ctx.find(savor::context::key::core::PRED_COUNT);
        auto itT = ctx.find(savor::context::key::core::PRED_TABLE);
        auto itB = ctx.find(savor::context::key::core::PRED_BASELINES);
        auto itHit = ctx.find(savor::context::key::core::RUN_HIT_BP_KEY);
        if (itN == ctx.end() || itT == ctx.end() || itB == ctx.end() || itHit == ctx.end()) return;
        const uint32_t n = std::get<uint32_t>(itN->second);
        const auto* tbl = std::get_if<std::string>(&itT->second);
        auto* bas = std::get_if<std::string>(&itB->second);
        const uint32_t hit = std::get<uint32_t>(itHit->second);
        if (!n || !tbl || !bas || !hit) return;
        using savor::pred::PredFlag;
        const auto* rec = reinterpret_cast<const pred::PredicateRecord*>(tbl->data());
        for (uint32_t i = 0; i < n; ++i) {
            const auto& r = rec[i];
            if (!r.has_flag(PredFlag::RhsIsDelta) || !r.has_flag(PredFlag::Active) || !record_has_baseline_bp(*tbl, r, hit)) continue;
            uint64_t vbits = 0;
            if (r.lhs_addrprog_offset && read_via_addrprog(host_, derived_.get(), *tbl, r.lhs_addrprog_offset, r.width, vbits)) {}
            else if (r.lhs_addr_key) { if (!router.read(static_cast<addr::AddrKey>(r.lhs_addr_key), r.width, vbits)) continue; }
            else {
                switch (r.width) {
                case 1: { uint8_t v = 0; if (!host_.readU8(r.lhs_addr, v)) continue; vbits = v; break; }
                case 2: { uint16_t v = 0; if (!host_.readU16(r.lhs_addr, v)) continue; vbits = v; break; }
                case 4: { uint32_t v = 0; if (!host_.readU32(r.lhs_addr, v)) continue; vbits = v; break; }
                case 8: { uint64_t v = 0; if (!host_.readU64(r.lhs_addr, v)) continue; vbits = v; break; }
                default: continue;
                }
            }
            std::memcpy(bas->data() + i * sizeof(uint64_t), &vbits, sizeof(uint64_t));
        }
    }
    void PhaseScriptVM::op_eval_predicates_at_hit_bp(PSContext& ctx, KeyHostRouter& router) {
        uint32_t total = 0; ctx.get(savor::context::key::core::PRED_TOTAL, total);
        uint32_t pass = 0; ctx.get(savor::context::key::core::PRED_PASSED, pass);
        auto itN = ctx.find(savor::context::key::core::PRED_COUNT);
        auto itT = ctx.find(savor::context::key::core::PRED_TABLE);
        auto itB = ctx.find(savor::context::key::core::PRED_BASELINES);
        auto itHit = ctx.find(savor::context::key::core::RUN_HIT_BP_KEY);
        if (itN == ctx.end() || itT == ctx.end() || itB == ctx.end() || itHit == ctx.end()) return;
        const uint32_t n = std::get<uint32_t>(itN->second);
        const auto* tbl = std::get_if<std::string>(&itT->second);
        const auto* bas = std::get_if<std::string>(&itB->second);
        const uint32_t hit = std::get<uint32_t>(itHit->second);
        if (!n || !tbl || !bas || !hit) return;
        using savor::pred::PredFlag;
        const auto* rec = reinterpret_cast<const pred::PredicateRecord*>(tbl->data());
        const uint8_t* bas_ptr = reinterpret_cast<const uint8_t*>(bas->data());
        for (uint32_t i = 0; i < n; ++i) {
            const auto& r = rec[i];
            if (!r.has_flag(PredFlag::Active) || (r.required_bp && r.required_bp != hit)) continue;
            uint64_t lhs = 0, rhs = 0;
            if (r.has_flag(PredFlag::LhsIsProg) && read_via_addrprog(host_, derived_.get(), *tbl, r.lhs_addrprog_offset, r.width, lhs)) {}
            else if (r.has_flag(PredFlag::LhsIsKey)) { if (!router.read(static_cast<addr::AddrKey>(r.lhs_addr_key), r.width, lhs)) continue; }
            else { switch (r.width) { case 1: { uint8_t v = 0; if (!host_.readU8(r.lhs_addr, v)) continue; lhs = v; break; } case 2: { uint16_t v = 0; if (!host_.readU16(r.lhs_addr, v)) continue; lhs = v; break; } case 4: { uint32_t v = 0; if (!host_.readU32(r.lhs_addr, v)) continue; lhs = v; break; } case 8: { uint64_t v = 0; if (!host_.readU64(r.lhs_addr, v)) continue; lhs = v; break; } default: continue; } }
            if (r.has_flag(PredFlag::RhsIsDelta)) { uint64_t cap = 0; std::memcpy(&cap, bas_ptr + i * sizeof(uint64_t), sizeof(uint64_t)); rhs = cap; }
            else if (r.has_flag(PredFlag::RhsIsProg) && read_via_addrprog(host_, derived_.get(), *tbl, r.rhs_addrprog_offset, r.width, rhs)) {}
            else if (r.has_flag(PredFlag::RhsIsKey)) { if (!router.read(static_cast<addr::AddrKey>(r.rhs_addr_key), r.width, rhs)) continue; }
            else rhs = r.rhs_imm;
            bool ok = false;
            switch (r.cmp) { case 0: ok = (lhs == rhs); break; case 1: ok = (lhs != rhs); break; case 2: ok = (lhs < rhs); break; case 3: ok = (lhs <= rhs); break; case 4: ok = (lhs > rhs); break; case 5: ok = (lhs >= rhs); break; default: ok = false; break; }
            std::string cmp_string = std::to_string(lhs) + " " + pred::get_cmp_string((pred::CmpOp)r.cmp) + " " + std::to_string(rhs);
            uint32_t progress;
            if (ctx.get(savor::context::key::core::PROGRESS_CORE_FLAGS, progress) && (progress & (uint32_t)CoreProgressFlags::PredicateProgress) != 0 && host_.getProgressSink()) {
                std::string msg = std::format("{} - {}", r.name, cmp_string);
                msg = std::string("Pred") + (ok ? "(Passed): " : "(Failed): ") + msg;
                host_.getProgressSink()(msg.c_str(), true);
            }
            ++total; if (ok) ++pass;
            if (!ok && r.has_flag(pred::PredFlag::AbortOnFail)) { ctx[savor::context::key::core::PRED_ABORT_RUN] = (uint32_t)1; break; }
        }
        ctx[savor::context::key::core::PRED_PASSED] = pass;
        ctx[savor::context::key::core::PRED_TOTAL] = total;
    }

    PSResult PhaseScriptVM::run(const PSJob& job)
    {
        PSResult R{};
        auto ctx_heap = std::make_unique<PSContext>(job.ctx);
        PSContext& ctx = *ctx_heap;
        struct MemoryWatchpointRunScope {
            DolphinWrapper& host;
            ~MemoryWatchpointRunScope() { host.clearMemoryWatchpoints(); }
        } memory_watchpoint_scope{ host_ };
        host_.clearMemoryWatchpoints();
        predicate_bp_keys_.clear();
        std::string _section = "Entry Point";
        // Always start by restoring the pre-captured snapshot for each job
        if (!load_snapshot()) return R;

        if (!configure_capture_from_context(ctx, R)) {
            return R;
        }

        ctx[savor::context::key::core::VI_FIRST] = (uint32_t)(host_.getViFieldCountApprox() & 0xFFFFFFFFull);

        if (derived_) derived_->on_init(ctx);
        DolphinKeyReader     mem1_reader(&host_);
        std::unique_ptr<KeyHostRouter> router;
        if (derived_) router = std::make_unique<KeyHostRouter>(&mem1_reader, derived_.get());
        else router = std::make_unique<KeyHostRouter>(&mem1_reader, nullptr);

        std::unordered_map<std::string, size_t> label_vm_pc_map;
        for (size_t i = 0; i < prog_.ops.size(); ++i) {
            if (prog_.ops[i].code == PSOpCode::LABEL) label_vm_pc_map[prog_.ops[i].label.name] = i;
        }

        for (size_t vm_pc = 0; vm_pc < prog_.ops.size(); ++vm_pc) {
            wait_for_visual_debug_gate();
            const auto& op = prog_.ops[vm_pc];
            SCLOGT("[VM] running op: %s", get_psop_desc(op).c_str());

            switch (op.code) {
            case PSOpCode::ARM_PHASE_BPS_ONCE: if (!op_arm_phase_bps_once()) return R; break;
            case PSOpCode::LOAD_SNAPSHOT: if (!op_load_snapshot(ctx)) return R; break;
            case PSOpCode::CAPTURE_SNAPSHOT: if (!op_capture_snapshot()) return R; break;
            case PSOpCode::REBOOT_CORE: if (!op_reboot_core(R, ctx)) return R; break;
            case PSOpCode::LABEL: op_label(); break;
            case PSOpCode::GOTO: op_goto(op, label_vm_pc_map, vm_pc, _section); break;
            case PSOpCode::GOTO_IF: op_goto_if(op, ctx, label_vm_pc_map, vm_pc, _section); break;
            case PSOpCode::GOTO_IF_KEYS: op_goto_if_keys(op, ctx, label_vm_pc_map, vm_pc, _section); break;
            case PSOpCode::SET_U32: op_set_u32(op, ctx); break;
            case PSOpCode::ADD_U32: op_add_u32(op, ctx); break;
            case PSOpCode::BUILD_TURN_INPUTPLAN_FROM_BATTLE_PATH: op_build_turn_inputplan_from_battle_path(ctx); break;
            case PSOpCode::MATERIALIZE_BATTLE_MACRO_STEPS: op_materialize_battle_macro_steps(ctx); break;
            case PSOpCode::MATERIALIZE_BATTLE_TURN_MACRO_STEPS: op_materialize_battle_turn_macro_steps(ctx); break;
            case PSOpCode::EXECUTE_BATTLE_MACRO_STEP: op_execute_battle_macro_step(ctx); break;
            case PSOpCode::APPLY_BATTLE_INPUTPLAN_FRAMES: op_apply_battle_inputplan_frames(ctx); break;
            case PSOpCode::STEP_FRAMES: op_step_frames(op); break;
            case PSOpCode::STEP_OPCODE: op_step_opcode(op); break;
            case PSOpCode::START_DETERMINISIC_RUN: op_start_deterministic_run(); break;
            case PSOpCode::END_DETERMINISTIC_RUN: op_end_deterministic_run(); break;
            case PSOpCode::RUN_UNTIL_BP: op_run_until_bp(ctx); break;
            case PSOpCode::RUN_UNTIL_BP_KEY: op_run_until_bp_key(op, ctx); break;
            case PSOpCode::RUN_UNTIL_DEBUG_STOP: op_run_until_debug_stop(ctx); break;
            case PSOpCode::ARM_MEMORY_WATCHPOINT: if (!op_arm_memory_watchpoint(op, R, ctx)) return R; break;
            case PSOpCode::CLEAR_MEMORY_WATCHPOINTS: op_clear_memory_watchpoints(); break;
            case PSOpCode::ARM_CAPTURE_MEMORY_WATCHPOINTS: if (!op_arm_capture_memory_watchpoints(R, ctx)) return R; break;
            case PSOpCode::RECORD_CURRENT_BP: op_record_current_bp(ctx); break;
            case PSOpCode::RECORD_TAS_INPUT_SAMPLE: op_record_tas_input_sample(ctx); break;
            case PSOpCode::READ_U8: if (!op_read_u8(op, R, ctx)) return R; break;
            case PSOpCode::READ_U16: if (!op_read_u16(op, R, ctx)) return R; break;
            case PSOpCode::READ_U32: if (!op_read_u32(op, R, ctx)) return R; break;
            case PSOpCode::WRITE_U32: if (!op_write_u32(op, R, ctx)) return R; break;
            case PSOpCode::READ_F32: if (!op_read_f32(op, R, ctx)) return R; break;
            case PSOpCode::READ_F64: if (!op_read_f64(op, R, ctx)) return R; break;
            case PSOpCode::CAPTURE_SEED_OVERRIDE: if (!op_capture_seed_override(R, ctx)) return R; break;
            case PSOpCode::GET_BATTLE_CONTEXT: op_get_battle_context(R, ctx); break;
            case PSOpCode::EMIT_RESULT: op_emit_result(op, R, ctx); break;
            case PSOpCode::RETURN_RESULT: if (op_return_result(op, R, ctx)) return R; break;
            case PSOpCode::APPLY_INPUT_FROM: if (!op_apply_input_from(op, R, ctx)) return R; break;
            case PSOpCode::SET_TIMEOUT: op_set_timeout(op, ctx); break;
            case PSOpCode::SET_TIMEOUT_FROM: op_set_timeout_from(op, ctx); break;
            case PSOpCode::MOVIE_PLAY_FROM: if (!op_movie_play_from(op, R, ctx)) return R; break;
            case PSOpCode::MOVIE_STOP: host_.endMoviePlaybackBlocking(); break;
            case PSOpCode::SAVE_SAVESTATE_FROM: if (!op_save_savestate_from(op, R, ctx)) return R; break;
            case PSOpCode::REQUIRE_DISC_GAMEID_FROM: if (!op_require_disc_gameid_from(op, R, ctx)) return R; break;
            case PSOpCode::ARM_BPS_FROM_PRED_TABLE: op_arm_bps_from_pred_table(ctx); break;
            case PSOpCode::CAPTURE_PRED_BASELINES: op_capture_pred_baselines(ctx, *router); break;
            case PSOpCode::EVAL_PREDICATES_AT_HIT_BP: op_eval_predicates_at_hit_bp(ctx, *router); break;
            default: break;
            }
        }
        ctx[savor::context::key::core::VI_LAST] = (uint32_t)(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
        R.ctx = ctx;
        R.ok = true;
        return R;
    }

    std::string get_psop_name(PSOpCode op)
    {
        switch (op) {
        case PSOpCode::ARM_PHASE_BPS_ONCE: return { "Arm Phase BPs Once" };
        case PSOpCode::LOAD_SNAPSHOT: return { "Load Snapshot" };
        case PSOpCode::CAPTURE_SNAPSHOT: return { "Capture Snapshot" };
        case PSOpCode::APPLY_INPUT_FROM: return { "Apply Input" };
        case PSOpCode::STEP_FRAMES: return { "Step Frames" };
        case PSOpCode::STEP_OPCODE: return { "Step Opcode" };
        case PSOpCode::RUN_UNTIL_BP: return { "Run Until BP" };
        case PSOpCode::RUN_UNTIL_BP_KEY: return { "Run Until BP Key" };
        case PSOpCode::RUN_UNTIL_DEBUG_STOP: return { "Run Until Debug Stop" };
        case PSOpCode::ARM_MEMORY_WATCHPOINT: return { "Arm Memory Watchpoint" };
        case PSOpCode::CLEAR_MEMORY_WATCHPOINTS: return { "Clear Memory Watchpoints" };
        case PSOpCode::ARM_CAPTURE_MEMORY_WATCHPOINTS: return { "Arm Capture Memory Watchpoints" };
        case PSOpCode::RECORD_CURRENT_BP: return { "Record Current BP" };
        case PSOpCode::READ_U8: return { "Read u8" };
        case PSOpCode::READ_U16: return { "Read u16" };
        case PSOpCode::READ_U32: return { "Read u32" };
        case PSOpCode::WRITE_U32: return { "Write u32" };
        case PSOpCode::READ_F32: return { "Read float" };
        case PSOpCode::READ_F64: return { "Read double" };
        case PSOpCode::SET_TIMEOUT: return { "Set Timeout" };
        case PSOpCode::SET_TIMEOUT_FROM: return { "Set Timeout" };
        case PSOpCode::EMIT_RESULT: return { "Emit result" };
        case PSOpCode::MOVIE_PLAY_FROM: return { "Play TAS Movie" };
        case PSOpCode::MOVIE_STOP: return { "Stop TAS Movie" };
        case PSOpCode::SAVE_SAVESTATE_FROM: return { "Save Savestate" };
        case PSOpCode::REQUIRE_DISC_GAMEID_FROM: return { "Require Disc ID" };
        case PSOpCode::BUILD_TURN_INPUTPLAN_FROM_BATTLE_PATH: return { "Build Turn Input From Actions" };
        case PSOpCode::MATERIALIZE_BATTLE_MACRO_STEPS: return { "Materialize Battle Macro Steps" };
        case PSOpCode::MATERIALIZE_BATTLE_TURN_MACRO_STEPS: return { "Materialize Battle Turn Macro Steps" };
        case PSOpCode::EXECUTE_BATTLE_MACRO_STEP: return { "Execute Battle Macro Step" };
        case PSOpCode::GET_BATTLE_CONTEXT: return { "Get Battle Context" };
        case PSOpCode::GC_SLOT_A_SET_FROM: return { "Set GC Memcard Slot A" };
        case PSOpCode::LABEL: return { "Set Label" };
        case PSOpCode::GOTO: return { "Goto Label" };
        case PSOpCode::GOTO_IF: return { "Constant Goto Label If" };
        case PSOpCode::GOTO_IF_KEYS: return { "Context Goto Label If" };
        case PSOpCode::RETURN_RESULT: return { "Return Result" };
        case PSOpCode::CAPTURE_PRED_BASELINES: return { "Capture Predicate Breakpoint Baselines" };
        case PSOpCode::ARM_BPS_FROM_PRED_TABLE: return { "Arm Breakpoints from Predicate Table" };
        case PSOpCode::EVAL_PREDICATES_AT_HIT_BP: return { "Evaulate Predicates at Hit BP" };
        case PSOpCode::SET_U32: return { "Set a u32 Context Value" };
        case PSOpCode::ADD_U32: return { "Add to a u32 Context Value" };
        case PSOpCode::APPLY_BATTLE_INPUTPLAN_FRAMES : return { "Apply Inputplan Frame from Context" };
        case PSOpCode::RECORD_TAS_INPUT_SAMPLE: return { "Record TAS Input Sample" };
        case PSOpCode::CAPTURE_SEED_OVERRIDE: return { "Capture Seed Override" };
        default:
            return { "Unknown Code" };
        }
    }

    std::string get_psop_desc(const PSOp& op)
    {
        std::ostringstream args;
        switch (op.code) {
        case PSOpCode::READ_U8:
        case PSOpCode::READ_U16:
        case PSOpCode::READ_U32:
        case PSOpCode::READ_F32:
        case PSOpCode::READ_F64:
            args << "addr=" << op.rd.addr << ", dst=" << key_desc(op.rd.dst);
            break;
        case PSOpCode::WRITE_U32:
            args << "addr=" << op.rd.addr << ", value_key=" << key_desc(op.rd.dst);
            break;
        case PSOpCode::APPLY_INPUT_FROM:
        case PSOpCode::SET_TIMEOUT_FROM:
        case PSOpCode::MOVIE_PLAY_FROM:
        case PSOpCode::SAVE_SAVESTATE_FROM:
        case PSOpCode::REQUIRE_DISC_GAMEID_FROM:
        case PSOpCode::GC_SLOT_A_SET_FROM:
        case PSOpCode::EMIT_RESULT:
        case PSOpCode::APPLY_BATTLE_INPUTPLAN_FRAMES:
            args << "key=" << key_desc(op.key.id);
            break;
        case PSOpCode::STEP_FRAMES:
            args << "n=" << op.step.n << ", disable_breakpoints=" << op.imm.v;
            break;
        case PSOpCode::STEP_OPCODE:
            args << "disable_breakpoints=" << op.imm.v;
            break;
        case PSOpCode::ARM_MEMORY_WATCHPOINT:
            args << "id=" << op.memwatch.id
                 << ", addr=" << op.memwatch.address
                 << ", addr_key=" << key_desc(op.memwatch.address_key)
                 << ", use_addr_key=" << op.memwatch.use_address_key
                 << ", size=" << op.memwatch.size
                 << ", access=" << op.memwatch.access;
            break;
        case PSOpCode::SET_TIMEOUT:
            args << "ms=" << op.imm.v;
            break;
        case PSOpCode::RUN_UNTIL_BP_KEY:
            args << "bp_key=" << op.imm.v;
            break;
        case PSOpCode::LABEL:
            args << "name=" << op.label.name;
            break;
        case PSOpCode::GOTO:
            args << "name=" << op.jmp.name;
            break;
        case PSOpCode::GOTO_IF:
            args << "key=" << key_desc(op.jcc.key)
                 << ", cmp=" << ps_cmp_to_string(op.jcc.cmp)
                 << ", imm=" << op.jcc.imm
                 << ", name=" << op.jcc.name;
            break;
        case PSOpCode::GOTO_IF_KEYS:
            args << "left=" << key_desc(op.jcc2.left)
                 << ", cmp=" << ps_cmp_to_string(op.jcc2.cmp)
                 << ", right=" << key_desc(op.jcc2.right)
                 << ", name=" << op.jcc2.name;
            break;
        case PSOpCode::RETURN_RESULT:
        case PSOpCode::SET_U32:
        case PSOpCode::ADD_U32:
            args << "key=" << key_desc(op.keyimm.key) << ", imm=" << op.keyimm.imm;
            break;
        default:
            break;
        }
        return get_psop_name(op.code) + ": [" + args.str() + "]";
    }

} // namespace savor
