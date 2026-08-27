#include "BattleSingleTurnModule.h"

#include "Runner/Runtime/ProgramKind.h"
#include "Core/Input/SoaBattle/BattleCommandCodec.h"
#include "Runner/Runtime/ProgramRuntime/Actions/CanonicalActionPayload.h"
#include "Runner/Runtime/DerivedState/DerivedStateTypes.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceReducers.h"
#include "Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "Runner/Runtime/ProgramRuntime/Composition/CompositionSupport.h"
#include "Runner/Runtime/ProgramRuntime/Composition/InteractionComposition.h"
#include "Runner/Runtime/ProgramRuntime/Composition/InputDeliveryComposition.h"
#include "Runner/Runtime/ProgramRuntime/ProgramRuntime.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "Runner/Runtime/ProgramRuntime/Registry/TypeSchemaRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Store/ProgramDefinitionStore.h"
#include "Runner/Runtime/ProgramRuntime/Verify/ProgramVerifier.h"
#include "Runner/Breakpoints/BpRegistry.h"
#include "Utils/Hash.h"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace savor::runtime::battlesingleturn {
namespace {

using namespace program;
using namespace program::composition;
using Builder = program::composition::detail::ModuleFragmentBuilder;

constexpr std::array<std::uint8_t, 4> kWireMagic{'B','S','T','1'};
constexpr std::string_view kCommonSchema = "soa.battle.single_turn.CommonInput";
constexpr std::uint32_t kCommonVersion = 1;
// The admitted Battle Plan permits up to 255 fake attacks. Each fake attack
// can perform a bounded 120-poll memory-change wait plus seven synchronized
// neutral frames, so the runtime budget must cover that declared worst case.
constexpr std::uint64_t kBattleInstructionBudget = 1'000'000;
constexpr std::uint64_t kBattleCallBudget = 4'096;
constexpr std::uint64_t kBattleActionBudget = 131'072;
constexpr std::uint64_t kBattleValueBudget = 262'144;
constexpr std::uint64_t kBattleTraceBudget = 1'000'000;

void SetDiagnostic(std::string* output, std::string value) { if (output) *output = std::move(value); }

class Writer {
public:
    void U8(std::uint8_t v){bytes.push_back(v);} void U32(std::uint32_t v){for(int s=0;s<32;s+=8)bytes.push_back(static_cast<std::uint8_t>(v>>s));}
    void Text(std::string_view v){U32(static_cast<std::uint32_t>(v.size()));bytes.insert(bytes.end(),v.begin(),v.end());}
    std::vector<std::uint8_t> bytes;
};
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> v):value(v){} bool U8(std::uint8_t& v){if(offset>=value.size())return false;v=value[offset++];return true;}bool U32(std::uint32_t& v){if(value.size()-offset<4)return false;v=0;for(int s=0;s<32;s+=8)v|=static_cast<std::uint32_t>(value[offset++])<<s;return true;}bool Text(std::string& v){std::uint32_t n=0;if(!U32(n)||n>32768||value.size()-offset<n)return false;v.assign(reinterpret_cast<const char*>(value.data()+offset),n);offset+=n;return true;}bool Done()const{return offset==value.size();}
private:std::span<const std::uint8_t> value;std::size_t offset=0;
};

std::vector<Byte> FrameBytes(const GCInputFrame& f){return {static_cast<Byte>(f.buttons),static_cast<Byte>(f.buttons>>8),f.main_x,f.main_y,f.c_x,f.c_y,f.trig_l,f.trig_r};}
bool ReadFrame(Reader& r,GCInputFrame& f){std::uint8_t lo=0,hi=0;return r.U8(lo)&&r.U8(hi)&&r.U8(f.main_x)&&r.U8(f.main_y)&&r.U8(f.c_x)&&r.U8(f.c_y)&&r.U8(f.trig_l)&&r.U8(f.trig_r)&&(f.buttons=static_cast<std::uint16_t>(lo|(hi<<8)),true);}

SchemaIdentity ExactSchema(std::string id,std::string contract){SchemaIdentity value{.canonical_id=std::move(id),.version=1};value.schema_hash=ContentHash256::FromHex(hash::sha256(contract.data(),contract.size())).value();return value;}
SchemaIdentity RequestSchema(){return ExactSchema("soa.battle.single_turn.Request","record Request/1(turn_index:u32,cumulative_fake_attacks_before:u32,plan:soa.battle.BattleTurnExecutionSpec/1,confirmed_seed_frame:runtime.input.InputFramePayload/1,save_request:runtime.action.savestate_save_immutable_artifact.Input/1)");}
std::string OutcomeContract(){std::string contract="enum soa.battle.single_turn.Outcome/1{";for(std::size_t index=0;index<BattleSingleTurnOutcomeDefinitionsV1.size();++index){if(index!=0)contract+=',';const auto&definition=BattleSingleTurnOutcomeDefinitionsV1[index];contract+=std::format("{}={}",definition.name,static_cast<std::int64_t>(definition.outcome));}contract+='}';return contract;}
std::vector<EnumMemberDefinition> OutcomeMembers(){std::vector<EnumMemberDefinition> members;members.reserve(BattleSingleTurnOutcomeDefinitionsV1.size());for(const auto&definition:BattleSingleTurnOutcomeDefinitionsV1)members.push_back({std::string(definition.name),static_cast<std::int64_t>(definition.outcome)});return members;}
SchemaIdentity OutcomeSchema(){return ExactSchema("soa.battle.single_turn.Outcome",OutcomeContract());}
SchemaIdentity ResultSchema(){return ExactSchema("soa.battle.single_turn.Result","record Result/1(outcome:Outcome,ending_rng:u32,vi_start:u64,vi_end:u64,pred_passed:u32,pred_total:u32,cumulative_fake_attacks:u32,has_context:bool,context:soa.battle.BattleContext/1)");}
TypeRef RequestType(){return TypeRef::Named(RequestSchema());}TypeRef OutcomeType(){return TypeRef::Named(OutcomeSchema());}TypeRef ResultType(){return TypeRef::Named(ResultSchema());}

class StaticWriter{public:explicit StaticWriter(std::array<char,4> m){for(char c:m)b.push_back(static_cast<Byte>(c));}void U8(std::uint8_t v){b.push_back(v);}void U32(std::uint32_t v){for(int s=0;s<32;s+=8)b.push_back(static_cast<Byte>(v>>s));}void Bool(bool v){U8(v?1:0);}void Text(std::string_view v){U32(static_cast<std::uint32_t>(v.size()));b.insert(b.end(),v.begin(),v.end());}void Hash(const ContentHash256& h){b.insert(b.end(),h.bytes.begin(),h.bytes.end());}std::vector<Byte> Finish(){return std::move(b);}private:std::vector<Byte>b;};

SemanticPointReference Point(std::string name,std::uint32_t pc){const bool field=name.starts_with("prebattle.");return {.capability_pack=field?capabilities::FieldPackIdentity():capabilities::BattlePackIdentity(),.canonical_id=(field?"soa.field.point.":"soa.battle.point.")+std::move(name),.kind=SemanticPointKind::ProgramCounter,.physical_pc=pc};}
SemanticPointReference CommandPoint(std::string name,std::uint32_t pc){return {.capability_pack=capabilities::BattleCommandPackIdentity(),.canonical_id="soa.battle.command.point."+std::move(name),.kind=SemanticPointKind::ProgramCounter,.physical_pc=pc};}
SemanticPointReference CommandPoint(std::string name,BPKey key){const auto*point=bp::BpRegistry::FindRuntime(key);if(!point||point->pc==0)throw std::logic_error("battle command semantic point is unavailable");return CommandPoint(std::move(name),point->pc);}
std::vector<Byte> ContinueConfig(){StaticWriter w({'C','U','C','2'});w.U8(1);w.U8(1);w.U8(static_cast<std::uint8_t>(ExecutionThrottlePolicy::RequireDisabled));w.U8(0);return w.Finish();}
std::vector<Byte> ObservationConfig(){StaticWriter w({'O','S','C','1'});w.Text("ending-rng");w.U8(0);w.Bool(false);w.U8(0);return w.Finish();}

void AddAction(Builder& b,CanonicalAction a){b.AddCapabilityImport(CanonicalRuntimePackIdentity());b.AddActionImport(CanonicalActionIdentity(a));for(const auto&s:CanonicalActionTypeSchemaClosure(a))b.AddTypeImport(s);}
InstructionTarget Action(CanonicalAction a){return {.kind=InstructionTargetKind::Action,.dependency=CanonicalActionIdentity(a)};}
InstructionTarget Reducer(CanonicalReducer r){return {.kind=InstructionTargetKind::Reducer,.dependency=CanonicalReducerIdentity(r)};}
ProgramValueId Need(std::optional<ProgramValueId> value,std::string_view what){if(!value)throw std::logic_error("battle.single_turn lowering failed: "+std::string(what));return *value;}
ProgramValueId Constant(Builder&b,ProgramFunction&f,BasicBlock&block,TypeRef type,LiteralPayload payload,std::string selector,ProgramScopeId scope={}){return Need(b.AddInstruction(f,block,InstructionOpcode::Constant,type,{},{},std::move(selector),LiteralValue{type,std::move(payload)},scope),"constant");}
ProgramValueId Bytes(Builder&b,ProgramFunction&f,BasicBlock&block,CanonicalRuntimeSchema schema,std::vector<Byte> bytes,std::string selector,ProgramScopeId scope={}){const auto type=CanonicalRuntimeType(schema);return Constant(b,f,block,type,std::move(bytes),std::move(selector),scope);}
ProgramValueId Construct(Builder&b,ProgramFunction&f,BasicBlock&block,TypeRef type,std::span<const ProgramValueId> fields,std::string selector,ProgramScopeId scope={}){return Need(b.AddInstruction(f,block,InstructionOpcode::RecordConstruct,type,fields,{},std::move(selector),std::nullopt,scope),"record");}
ProgramValueId Project(Builder&b,ProgramFunction&f,BasicBlock&block,ProgramValueId value,TypeRef type,std::string field,ProgramScopeId scope={}){return Need(b.AddInstruction(f,block,InstructionOpcode::RecordProject,type,std::array{value},{},std::move(field),std::nullopt,scope),"projection");}
ProgramValueId Optional(Builder&b,ProgramFunction&f,BasicBlock&block,CanonicalRuntimeSchema schema,std::optional<ProgramValueId> value,std::string selector,ProgramScopeId scope={}){std::vector<ProgramValueId> operands;if(value)operands.push_back(*value);return Need(b.AddInstruction(f,block,InstructionOpcode::OptionalConstruct,CanonicalRuntimeType(schema),operands,{},std::move(selector),std::nullopt,scope),"optional");}
ProgramValueId Await(Builder&b,ProgramFunction&f,BasicBlock&block,CanonicalAction action,ProgramValueId request,std::string selector,ProgramScopeId scope={}){return Need(b.AddInstruction(f,block,InstructionOpcode::AwaitAction,CanonicalActionOutputType(action),std::array{request},Action(action),std::move(selector),std::nullopt,scope),"action");}

ProgramValueId RequirePc(Builder&b,ProgramFunction&f,BasicBlock&block,std::uint32_t pc,std::string selector){const auto expected=Constant(b,f,block,TypeRef::Builtin(BuiltinType::U64),static_cast<std::uint64_t>(pc),selector+"/expected");const auto request=Construct(b,f,block,CanonicalActionInputType(CanonicalAction::ExecutionRequirePausedPc),std::array{expected},selector+"/request");return Await(b,f,block,CanonicalAction::ExecutionRequirePausedPc,request,selector+"/require");}

ProgramValueId ContinueTo(Builder&b,ProgramFunction&f,BasicBlock&block,std::span<const SemanticPointReference> points,std::string selector,ProgramScopeId parent_scope={}){const auto scope=b.NewScope();(void)b.AddInstruction(f,block,InstructionOpcode::EnterScope,std::nullopt,{}, {},selector+"/scope",std::nullopt,scope);const auto point_set=Bytes(b,f,block,CanonicalRuntimeSchema::SemanticPointSet,EncodeSemanticPointSetV1(points),selector+"/semantic-points",scope);const auto no_input=Optional(b,f,block,CanonicalRuntimeSchema::OptionalInputExecutionBinding,std::nullopt,selector+"/no-input",scope);const auto no_movie=Optional(b,f,block,CanonicalRuntimeSchema::OptionalMoviePlaybackSession,std::nullopt,selector+"/no-movie",scope);const auto no_count=Optional(b,f,block,CanonicalRuntimeSchema::OptionalMovieInputCount,std::nullopt,selector+"/no-count",scope);const auto one=Constant(b,f,block,TypeRef::Builtin(BuiltinType::U64),std::uint64_t{1},selector+"/one",scope);const auto no_verify=Constant(b,f,block,TypeRef::Builtin(BuiltinType::Bool),false,selector+"/no-verify",scope);const auto cc=Bytes(b,f,block,CanonicalRuntimeSchema::ContinueUntilStaticConfig,ContinueConfig(),selector+"/continue-config",scope);const std::array fields{point_set,no_input,no_movie,no_count,one,no_verify,cc};const auto request=Construct(b,f,block,CanonicalActionInputType(CanonicalAction::ExecutionContinueUntil),fields,selector+"/continue-request",scope);const auto result=Await(b,f,block,CanonicalAction::ExecutionContinueUntil,request,selector+"/continue");(void)b.AddInstruction(f,block,InstructionOpcode::ExitScope,std::nullopt,{}, {},selector+"/release-scope",std::nullopt,scope);(void)parent_scope;return result;}

ProgramValueId CaptureContext(Builder&b,ProgramFunction&f,BasicBlock&block,ProgramValueId epoch,ProgramValueId pc,std::string selector){const std::array fields{epoch,pc};const auto request=Construct(b,f,block,TypeRef::Named(capabilities::BattleCaptureContextRequestSchemaIdentity()),fields,selector+"/request");return Need(b.AddInstruction(f,block,InstructionOpcode::AwaitAction,TypeRef::Named(capabilities::BattleContextSchemaIdentity()),std::array{request},{.kind=InstructionTargetKind::Action,.dependency=capabilities::BattleCaptureContextActionIdentity()},selector+"/capture"),"context capture");}

ProgramValueId ReadRng(Builder&b,ProgramFunction&f,BasicBlock&block,ProgramValueId stop,std::string selector){const auto optional=Optional(b,f,block,CanonicalRuntimeSchema::OptionalContinueUntilResult,stop,selector+"/stop");const auto address=Constant(b,f,block,TypeRef::Builtin(BuiltinType::U64),static_cast<std::uint64_t>(RngSeedAddress),selector+"/address");const auto config=Bytes(b,f,block,CanonicalRuntimeSchema::ObservationStaticConfig,ObservationConfig(),selector+"/config");const std::array fields{optional,address,config};const auto request=Construct(b,f,block,CanonicalActionInputType(CanonicalAction::GuestReadU32),fields,selector+"/request");return Await(b,f,block,CanonicalAction::GuestReadU32,request,selector+"/read");}

ProgramValueId ApplyConfirmedSeedFrame(
    Builder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    ProgramValueId frame)
{
    const std::array endpoint{
        Point("prebattle.AfterRandSeedSet", AfterRandSeedSetPc)};
    const auto delivered = LowerSynchronizedFrameDelivery(
        builder,
        function,
        block,
        frame,
        endpoint,
        "seed");
    if (!delivered)
        throw std::logic_error(
            "battle.single_turn seed-frame delivery lowering failed");
    return delivered->stop;
}

InteractionDefinition CommandInteraction()
{
    const auto receipt = CanonicalActionOutputType(
        CanonicalAction::ExecutionContinueUntil);
    InteractionDefinition definition{
        .canonical_id = "soa.battle.command_entry",
        .revision = 3,
        .source_name = "BattleSingleTurnModule",
        .parameters = {
            {"state", TypeRef::Named(capabilities::BattleCommandStateSchemaIdentity())},
            {"press_a", CanonicalRuntimeType(CanonicalRuntimeSchema::InputFramePayload)},
            {"press_b", CanonicalRuntimeType(CanonicalRuntimeSchema::InputFramePayload)},
            {"press_up", CanonicalRuntimeType(CanonicalRuntimeSchema::InputFramePayload)},
            {"press_down", CanonicalRuntimeType(CanonicalRuntimeSchema::InputFramePayload)},
        },
        .state_type = TypeRef::Named(capabilities::BattleCommandStateSchemaIdentity()),
        .output_type = TypeRef::Named(
            capabilities::BattleCommandReceiptSchemaIdentity()),
        .lease_type = CanonicalActionOutputType(CanonicalAction::InputAcquireLease),
        .point_receipt_type = receipt,
        .input_execution_binding_type = CanonicalActionOutputType(CanonicalAction::InputApplyState),
        .segment_result_type = receipt,
        .adaptive_transition_type = TypeRef::Named(capabilities::BattleCommandTransitionSchemaIdentity()),
        .adaptive_segment_id_type = TypeRef::Named(capabilities::BattleCommandSegmentSchemaIdentity()),
        .adaptive_selection = InteractionAdaptiveSelectionMap{
            .complete = capabilities::BattleCommandSegmentValue(
                capabilities::BattleCommandSegment::Complete),
        },
        .initialize_reducer = CanonicalReducerIdentity(CanonicalReducer::BattleCommandInteractionInitialize),
        .advance_reducer = CanonicalReducerIdentity(CanonicalReducer::BattleCommandInteractionAdvance),
        .finalize_reducer = CanonicalReducerIdentity(CanonicalReducer::BattleCommandInteractionFinalize),
        .actions = {
            CanonicalActionIdentity(CanonicalAction::InputAcquireLease),
            CanonicalActionIdentity(CanonicalAction::InputApplyState),
            CanonicalActionIdentity(CanonicalAction::ExecutionContinueUntil),
            CanonicalActionIdentity(CanonicalAction::ExecutionStepFrames),
        },
        .segments = {},
        .first_segment = "await-input-ready",
        .budgets = {
            .maximum_instructions = kBattleInstructionBudget,
            .maximum_calls = kBattleCallBudget,
            .maximum_call_depth = 8,
            .maximum_action_requests = kBattleActionBudget,
            .maximum_values = kBattleValueBudget,
            .maximum_value_bytes = 16 * 1024 * 1024,
            .maximum_trace_events = kBattleTraceBudget,
        },
    };

    const auto held = [&](std::string id,
                          SemanticPointReference gate,
                          std::size_t input_parameter) {
        return InteractionSegmentDefinition{
            .canonical_id = std::move(id),
            .gate_alternatives = {std::move(gate)},
            .requested_input_parameter = input_parameter,
            .input_kind = InteractionInputKind::Held,
            .fail_on_movie_end = true,
            .completion_mapper = CanonicalReducerIdentity(
                CanonicalReducer::BattleCommandInteractionCompleteSegment),
        };
    };
    const auto neutral = [&](std::string id, SemanticPointReference gate) {
        return InteractionSegmentDefinition{
            .canonical_id = std::move(id),
            .gate_alternatives = {std::move(gate)},
            .requested_input_parameter = 0,
            .input_kind = InteractionInputKind::Neutral,
            .fail_on_movie_end = true,
            .completion_mapper = CanonicalReducerIdentity(
                CanonicalReducer::BattleCommandInteractionCompleteSegment),
        };
    };
    const auto add = [&](capabilities::BattleCommandSegment selection,
                         InteractionSegmentDefinition segment) {
        const auto inserted = definition.adaptive_selection->segments.emplace(
            capabilities::BattleCommandSegmentValue(selection),
            segment.canonical_id);
        if (!inserted.second)
            throw std::logic_error("duplicate Battle command segment selection");
        definition.segments.push_back(std::move(segment));
    };

    add(capabilities::BattleCommandSegment::AwaitInputReady, neutral(
        "await-input-ready",
        CommandPoint("BattleMacroInputReadyGate", bp::battle::BattleMacroInputReadyGate)));
    auto fake_accept = held(
        "fake-accept",
        CommandPoint("BattleMacroMainMenuAcceptDispatch", bp::battle::BattleMacroMainMenuAcceptDispatch),
        1);
    fake_accept.held_through_successor = CommandPoint(
        "BattleMacroEnemyTargetReady", bp::battle::BattleMacroEnemyTargetReady);
    fake_accept.memory_change_observation = CanonicalActionIdentity(
        CanonicalAction::GuestReadU32);
    fake_accept.memory_change_address = RngSeedAddress;
    fake_accept.memory_change_value_type = TypeRef::Builtin(BuiltinType::U32);
    fake_accept.maximum_memory_polls = MaximumFakeAttackMemoryPolls;
    fake_accept.post_gate_neutral_frames = FakeAttackTargetNeutralFrames;
    add(capabilities::BattleCommandSegment::FakeAccept, std::move(fake_accept));
    add(capabilities::BattleCommandSegment::FakeBack, held(
        "fake-back",
        CommandPoint("BattleMacroInputReadyGate", bp::battle::BattleMacroInputReadyGate),
        2));
    add(capabilities::BattleCommandSegment::AttackAccept, held(
        "attack-accept",
        CommandPoint("BattleMacroMainMenuAcceptDispatch", bp::battle::BattleMacroMainMenuAcceptDispatch),
        1));
    add(capabilities::BattleCommandSegment::AttackTargetReady, neutral(
        "attack-target-ready",
        CommandPoint("BattleMacroEnemyTargetReady", bp::battle::BattleMacroEnemyTargetReady)));
    add(capabilities::BattleCommandSegment::AttackTargetReadyConfirm, neutral(
        "attack-target-ready-confirm",
        CommandPoint("BattleMacroEnemyTargetReady", bp::battle::BattleMacroEnemyTargetReady)));
    add(capabilities::BattleCommandSegment::AttackTargetDown, held(
        "attack-target-down",
        CommandPoint("BattleMacroEnemyTargetMoveDownAccepted", bp::battle::BattleMacroEnemyTargetMoveDownAccepted),
        4));
    add(capabilities::BattleCommandSegment::AttackTargetReadyBetween, neutral(
        "attack-target-ready-between",
        CommandPoint("BattleMacroEnemyTargetReady", bp::battle::BattleMacroEnemyTargetReady)));
    add(capabilities::BattleCommandSegment::AttackTargetAccept, held(
        "attack-target-accept",
        CommandPoint("BattleMacroEnemyTargetFinalized", bp::battle::BattleMacroEnemyTargetFinalized),
        1));
    add(capabilities::BattleCommandSegment::MainMenuMoveUp, held(
        "main-menu-up",
        CommandPoint("BattleMacroMainMenuMoveHigher", bp::battle::BattleMacroMainMenuMoveHigher),
        3));
    add(capabilities::BattleCommandSegment::MainMenuMoveDown, held(
        "main-menu-down",
        CommandPoint("BattleMacroMainMenuMoveLower", bp::battle::BattleMacroMainMenuMoveLower),
        4));
    add(capabilities::BattleCommandSegment::MainMenuTransition, neutral(
        "main-menu-transition",
        CommandPoint("BattleMacroCommandTransitionDone", bp::battle::BattleMacroCommandTransitionDone)));
    add(capabilities::BattleCommandSegment::DirectCommandAccept, held(
        "direct-command-accept",
        CommandPoint("BattleMacroDirectCommandQueued", bp::battle::BattleMacroDirectCommandQueued),
        1));
    add(capabilities::BattleCommandSegment::AwaitNextInputReady, neutral(
        "await-next-input-ready",
        CommandPoint("BattleMacroInputReadyGate", bp::battle::BattleMacroInputReadyGate)));
    add(capabilities::BattleCommandSegment::AwaitTurnReady, neutral(
        "await-turn-ready",
        Point("TurnIsReady", TurnIsReadyPc)));
    return definition;
}

struct LoweredPredicateMember
{
    const predicates::PredicateGroupMemberV1* member = nullptr;
    const predicates::PredicateExecutionBindingV1* binding = nullptr;
    std::string semantic_hook_id;
    ProgramFunctionId function;
    SchemaIdentity evaluation_schema;
    std::optional<ProgramFunctionId> guard_function;
    std::optional<SchemaIdentity> guard_evaluation_schema;
};

SchemaIdentity PredicateEvaluationSchema(
    const PredicateDefinition& definition)
{
    return program::composition::ExactSchema(
        definition.canonical_id + ".Evaluation",
        definition.revision,
        "enum PredicateEvaluation{Passed=0,Failed=1}");
}

const predicates::PredicateExecutionBindingV1* FindPredicateBinding(
    const predicates::PredicateExecutionPackageV1& package,
    std::int64_t revision_id)
{
    const auto found = std::ranges::find(
        package.execution_bindings,
        revision_id,
        &predicates::PredicateExecutionBindingV1::execution_binding_revision_id);
    return found == package.execution_bindings.end() ? nullptr : &*found;
}

std::vector<LoweredPredicateMember> LowerActivePredicates(
    const predicates::PredicateExecutionPackageV1& package,
    ProgramModule& module)
{
    std::vector<LoweredPredicateMember> lowered;
    for (const auto& member : package.group.members)
    {
        const auto* binding = FindPredicateBinding(
            package, member.execution_binding_revision_id);
        if (!binding)
            throw std::logic_error("active predicate execution binding is unavailable");
        for (const auto& hook_id : member.semantic_hook_ids)
        {
            PredicateEvaluationPolicy policy{
                .predicate_group_revision_id =
                    package.group.predicate_group_revision_id,
                .execution_binding_revision_id =
                    member.execution_binding_revision_id,
                .member_ordinal = member.ordinal,
                .semantic_point_id = hook_id,
                .reaction = member.reaction,
                .emit_evidence = member.emit_evidence ||
                    member.reaction == PredicateReaction::AbortOnFail,
                .participates_in_aggregation =
                    member.participates_in_aggregation,
            };
            const auto result = LowerPredicate(
                binding->definition.definition, policy, module);
            if (!result || !result.function)
                throw std::logic_error("active predicate lowering failed");
            LoweredPredicateMember item{
                .member = &member,
                .binding = binding,
                .semantic_hook_id = hook_id,
                .function = *result.function,
                .evaluation_schema = PredicateEvaluationSchema(
                    binding->definition.definition),
            };
            if (member.guard_execution_binding_revision_id)
            {
                const auto* guard = FindPredicateBinding(
                    package, *member.guard_execution_binding_revision_id);
                if (!guard)
                    throw std::logic_error(
                        "GuardOnce predicate execution binding is unavailable");
                auto guard_policy = policy;
                guard_policy.execution_binding_revision_id =
                    guard->execution_binding_revision_id;
                guard_policy.emit_evidence = false;
                guard_policy.reaction = PredicateReaction::RecordAndContinue;
                const auto guard_result = LowerPredicate(
                    guard->definition.definition, guard_policy, module);
                if (!guard_result || !guard_result.function)
                    throw std::logic_error("GuardOnce predicate lowering failed");
                item.guard_function = *guard_result.function;
                item.guard_evaluation_schema = PredicateEvaluationSchema(
                    guard->definition.definition);
            }
            lowered.push_back(std::move(item));
        }
    }
    return lowered;
}

std::optional<LiteralPayload> ZeroPayload(const TypeRef& type)
{
    if (type.is_named()) return std::nullopt;
    switch (type.builtin)
    {
    case BuiltinType::Unit: return UnitValue{};
    case BuiltinType::Bool: return false;
    case BuiltinType::U8: return std::uint8_t{};
    case BuiltinType::U16: return std::uint16_t{};
    case BuiltinType::U32: return std::uint32_t{};
    case BuiltinType::U64: return std::uint64_t{};
    case BuiltinType::I32: return std::int32_t{};
    case BuiltinType::I64: return std::int64_t{};
    case BuiltinType::F32: return float{};
    case BuiltinType::F64: return double{};
    }
    return std::nullopt;
}

CanonicalAction GuestReadAction(const TypeRef& type)
{
    if (type == TypeRef::Builtin(BuiltinType::U8)) return CanonicalAction::GuestReadU8;
    if (type == TypeRef::Builtin(BuiltinType::U16)) return CanonicalAction::GuestReadU16;
    if (type == TypeRef::Builtin(BuiltinType::U32)) return CanonicalAction::GuestReadU32;
    if (type == TypeRef::Builtin(BuiltinType::U64)) return CanonicalAction::GuestReadU64;
    throw std::logic_error("unsupported predicate guest-read type");
}

const predicates::PredicateHookPointV1* FindHook(
    const predicates::PredicateHookContractV1& contract,
    std::string_view canonical_id)
{
    const auto found = std::ranges::find(
        contract.points,
        canonical_id,
        &predicates::PredicateHookPointV1::canonical_id);
    return found == contract.points.end() ? nullptr : &*found;
}

SemanticPointReference SemanticPointForHook(
    const predicates::PredicateHookPointV1& hook)
{
    const auto* registered = bp::BpRegistry::FindRuntime(hook.pc);
    if (!registered || bp::domain_of(registered->key) != bp::BPDomain::Battle)
        throw std::logic_error("predicate hook is not a registered Battle point");
    return Point(registered->name, hook.pc);
}

ProgramValueId HookReceiptField(
    Builder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    ProgramValueId receipt,
    std::uint32_t hook_pc,
    const TypeRef& type,
    std::string_view field,
    std::string selector)
{
    if (field == "vi_count")
    {
        const auto paused = RequirePc(
            builder, function, block, hook_pc, selector + "/paused");
        return Project(
            builder, function, block, paused, type, "vi_count",
            {});
    }
    return Project(
        builder, function, block, receipt, type, std::string(field));
}

ProgramValueId AcquireObservation(
    Builder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    ProgramValueId receipt,
    std::uint32_t hook_pc,
    const predicates::PredicateWitnessSourceBindingV1& observation,
    std::string selector)
{
    using Kind = predicates::PredicateObservationSourceKindV1;
    if (observation.observation_source_kind == Kind::HookReceipt)
        return HookReceiptField(
            builder, function, block, receipt, hook_pc,
            observation.value_type, observation.source_field,
            std::move(selector));
    if (observation.observation_source_kind == Kind::GuestAddress)
    {
        const auto action = GuestReadAction(observation.value_type);
        const auto optional = Optional(
            builder, function, block,
            CanonicalRuntimeSchema::OptionalContinueUntilResult,
            receipt, selector + "/stop");
        const auto address = Constant(
            builder, function, block, TypeRef::Builtin(BuiltinType::U64),
            *observation.pinned_guest_address, selector + "/address");
        const auto config = Bytes(
            builder, function, block,
            CanonicalRuntimeSchema::ObservationStaticConfig,
            ObservationConfig(), selector + "/config");
        const std::array fields{optional, address, config};
        const auto request = Construct(
            builder, function, block, CanonicalActionInputType(action),
            fields, selector + "/request");
        return Await(
            builder, function, block, action, request,
            selector + "/read");
    }
    if (!observation.source)
        throw std::logic_error("predicate observation dependency is unavailable");
    const InstructionTarget target{
        .kind = observation.observation_source_kind == Kind::RegisteredQuery
            ? InstructionTargetKind::Action
            : InstructionTargetKind::Reducer,
        .dependency = *observation.source,
    };
    if (observation.observation_source_kind == Kind::RegisteredQuery)
        builder.AddActionImport(*observation.source);
    else
        builder.AddReducerImport(*observation.source);
    if (observation.observation_source_kind == Kind::RegisteredQuery &&
        (*observation.source == capabilities::BattleDerivedTurnEntryActionIdentity() ||
         *observation.source == capabilities::BattleDerivedTurnOrderActionIdentity() ||
         *observation.source == capabilities::BattleDerivedRewardsActionIdentity()))
    {
        const auto freshness_schema =
            capabilities::BattleDerivedFreshnessSchemaIdentity();
        const auto freshness = Constant(
            builder, function, block, TypeRef::Named(freshness_schema),
            EnumValue{
                freshness_schema,
                static_cast<std::int64_t>(
                    observation.source_kind ==
                            predicates::PredicateWitnessSourceKindV1::DerivedStateQuery
                        ? derived::DerivedStateFreshness::LatestInItem
                        : derived::DerivedStateFreshness::SameRoutedEvent)},
            selector + "/freshness");
        const auto routed = Optional(
            builder, function, block,
            CanonicalRuntimeSchema::OptionalContinueUntilResult,
            observation.source_kind ==
                    predicates::PredicateWitnessSourceKindV1::DerivedStateQuery
                ? std::optional<ProgramValueId>{}
                : std::optional<ProgramValueId>{receipt},
            selector + "/routed-stop");
        const std::array fields{freshness, routed};
        const auto request = Construct(
            builder, function, block,
            TypeRef::Named(
                capabilities::BattleDerivedQueryRequestSchemaIdentity()),
            fields, selector + "/request");
        return Need(builder.AddInstruction(
            function, block, InstructionOpcode::AwaitAction,
            observation.value_type, std::array{request}, target,
            std::move(selector)), "derived predicate observation");
    }
    return Need(builder.AddInstruction(
        function, block,
        observation.observation_source_kind == Kind::RegisteredQuery
            ? InstructionOpcode::AwaitAction
            : InstructionOpcode::CallReducer,
        observation.value_type, std::array{receipt}, target,
        std::move(selector)), "predicate observation");
}

void LowerTurnExecution(
    Builder& builder,
    ProgramFunction& function,
    BasicBlock& execute,
    ProgramFunctionId interaction_function,
    ProgramValueId interaction_state,
    ProgramValueId context,
    ProgramValueId plan,
    ProgramValueId cumulative_before,
    ProgramValueId vi_start,
    ProgramValueId save_request,
    const predicates::PredicateExecutionPackageV1& package,
    const std::vector<LoweredPredicateMember>& lowered_members)
{
    const bool has_abort_on_fail = std::ranges::any_of(
        lowered_members, [](const LoweredPredicateMember& lowered) {
            return lowered.member->reaction == PredicateReaction::AbortOnFail;
        });
    const auto receipt_type = CanonicalActionOutputType(
        CanonicalAction::ExecutionContinueUntil);
    GCInputFrame press_a{}; press_a.A();
    GCInputFrame press_b{}; press_b.B();
    GCInputFrame press_up{}; press_up.DUp();
    GCInputFrame press_down{}; press_down.DDown();
    const auto input_type = CanonicalRuntimeType(
        CanonicalRuntimeSchema::InputFramePayload);
    const auto a = Constant(builder, function, execute, input_type,
        FrameBytes(press_a), "command-entry/input/a");
    const auto b = Constant(builder, function, execute, input_type,
        FrameBytes(press_b), "command-entry/input/b");
    const auto up = Constant(builder, function, execute, input_type,
        FrameBytes(press_up), "command-entry/input/up");
    const auto down = Constant(builder, function, execute, input_type,
        FrameBytes(press_down), "command-entry/input/down");
    const auto command_receipt = Need(builder.AddInstruction(
        function, execute, InstructionOpcode::CallLocal,
        TypeRef::Named(capabilities::BattleCommandReceiptSchemaIdentity()),
        std::array{interaction_state, a, b, up, down},
        {.kind = InstructionTargetKind::LocalFunction,
         .local_function = interaction_function},
        "command-entry/interaction"), "command interaction");
    const auto ready_receipt = Project(
        builder, function, execute, command_receipt, receipt_type,
        "terminal");
    const auto fake_this = Project(
        builder, function, execute, plan,
        TypeRef::Builtin(BuiltinType::U32), "fake_attack_count");
    const auto cumulative = Need(builder.AddInstruction(
        function, execute, InstructionOpcode::AddChecked,
        TypeRef::Builtin(BuiltinType::U32),
        std::array{cumulative_before, fake_this}, {},
        "fake-attacks/cumulative"), "fake-attack accounting");

    struct BaselineRef
    {
        std::int64_t binding_revision_id = 0;
        std::uint32_t witness_ordinal = 0;
        const predicates::PredicateWitnessSourceBindingV1* source = nullptr;
    };
    std::vector<BaselineRef> baselines;
    for (const auto& binding : package.execution_bindings)
        for (const auto& witness : binding.witnesses)
            if (witness.source_kind ==
                predicates::PredicateWitnessSourceKindV1::BaselineObservation)
                baselines.push_back({binding.execution_binding_revision_id,
                    witness.witness_ordinal, &witness});

    std::vector<TypeRef> state_types{
        TypeRef::Builtin(BuiltinType::U32), // pred_passed
        TypeRef::Builtin(BuiltinType::U32), // pred_total
    };
    std::map<std::uint32_t, std::size_t> check_state;
    for (const auto& member : package.group.members)
    {
        check_state.emplace(member.ordinal, state_types.size());
        state_types.push_back(TypeRef::Builtin(BuiltinType::U32)); // hook hits
        state_types.push_back(TypeRef::Builtin(BuiltinType::Bool)); // completed
    }
    std::map<std::pair<std::int64_t, std::uint32_t>, std::size_t>
        baseline_state;
    for (const auto& baseline : baselines)
    {
        baseline_state.emplace(
            std::pair{baseline.binding_revision_id, baseline.witness_ordinal},
            state_types.size());
        state_types.push_back(TypeRef::Builtin(BuiltinType::Bool));
        if (!ZeroPayload(baseline.source->value_type))
            throw std::logic_error("predicate baseline type has no canonical empty state");
        state_types.push_back(baseline.source->value_type);
    }

    std::vector<ProgramValueId> initial_state;
    initial_state.reserve(state_types.size());
    for (std::size_t index = 0; index < state_types.size(); ++index)
    {
        const auto zero = ZeroPayload(state_types[index]);
        if (!zero) throw std::logic_error("predicate state cannot be initialized");
        initial_state.push_back(Constant(
            builder, function, execute, state_types[index], *zero,
            std::format("predicate/state/{}/initial", index)));
    }

    const auto state_arguments = [&](bool with_receipt)
    {
        std::vector<ValueDefinition> arguments;
        arguments.reserve(state_types.size() + (with_receipt ? 1u : 0u));
        if (with_receipt) arguments.push_back(builder.NewArgument(receipt_type));
        for (const auto& type : state_types)
            arguments.push_back(builder.NewArgument(type));
        return arguments;
    };
    const auto ids = [](std::span<const ValueDefinition> values,
                        std::size_t first = 0)
    {
        std::vector<ProgramValueId> result;
        result.reserve(values.size() - first);
        for (std::size_t i = first; i < values.size(); ++i)
            result.push_back(values[i].id);
        return result;
    };
    const auto edge_values = [](ProgramValueId receipt,
                                std::span<const ProgramValueId> state)
    {
        std::vector<ProgramValueId> values;
        values.reserve(state.size() + 1);
        values.push_back(receipt);
        values.insert(values.end(), state.begin(), state.end());
        return values;
    };

    std::map<std::string, const predicates::PredicateHookPointV1*> hooks_by_id;
    const auto require_hook = [&](std::string_view id)
    {
        const auto* hook = FindHook(package.hook_contract, id);
        if (!hook) throw std::logic_error("predicate hook contract drifted");
        hooks_by_id.emplace(hook->canonical_id, hook);
    };
    for (const auto& lowered : lowered_members)
        require_hook(lowered.semantic_hook_id);
    for (const auto& baseline : baselines)
        require_hook(baseline.source->baseline_capture_hook_id);
    for (const auto pc : {TurnIsReadyPc, TurnInputsPc, VictoryPc, DefeatPc})
    {
        const auto found = std::ranges::find(
            package.hook_contract.points, pc,
            &predicates::PredicateHookPointV1::pc);
        if (found == package.hook_contract.points.end())
            throw std::logic_error("required Battle terminal is absent from predicate hook contract");
        hooks_by_id.emplace(found->canonical_id, &*found);
    }
    std::vector<const predicates::PredicateHookPointV1*> hooks;
    for (const auto& [id, hook] : hooks_by_id)
    {
        (void)id;
        hooks.push_back(hook);
    }
    std::vector<SemanticPointReference> semantic_points;
    semantic_points.reserve(hooks.size());
    for (const auto* hook : hooks)
        semantic_points.push_back(SemanticPointForHook(*hook));

    auto loop_arguments = state_arguments(true);
    auto& loop = builder.AddBlock(function, loop_arguments);
    std::vector<ProgramValueId> initial_edge{ready_receipt};
    initial_edge.insert(initial_edge.end(), initial_state.begin(), initial_state.end());
    builder.SetTerminator(function, execute, {
        .kind = TerminatorKind::Branch,
        .edges = {{.target = loop.id, .arguments = initial_edge}},
    }, "predicate/enter-hooks");

    auto continuation_arguments = state_arguments(false);
    auto& continuation = builder.AddBlock(function, continuation_arguments);
    auto rejection_arguments = state_arguments(true);
    auto& rejection = builder.AddBlock(function, rejection_arguments);
    auto& unknown = builder.AddBlock(function);
    builder.SetTerminator(function, unknown, {
        .kind = TerminatorKind::StructuredFail,
        .failure = StructuredFailure{
            "battle_hook_unexpected",
            "Battle execution stopped at a point outside the admitted hook set"},
    }, "predicate/unexpected-hook");

    std::map<std::string, ProgramBlockId> hook_blocks;
    for (const auto* hook : hooks)
    {
        auto arguments = state_arguments(true);
        auto& block = builder.AddBlock(function, arguments);
        hook_blocks.emplace(hook->canonical_id, block.id);
    }

    BasicBlock* dispatch = &loop;
    for (std::size_t index = 0; index < hooks.size(); ++index)
    {
        BasicBlock* current_dispatch = dispatch;
        const auto receipt = current_dispatch->arguments[0].id;
        const auto state = ids(current_dispatch->arguments, 1);
        const auto pc = Project(
            builder, function, *current_dispatch, receipt,
            TypeRef::Builtin(BuiltinType::U32), "pc");
        const auto expected = Constant(
            builder, function, *current_dispatch,
            TypeRef::Builtin(BuiltinType::U32), hooks[index]->pc,
            std::format("predicate/dispatch/{}/expected", index));
        const auto matched = Need(builder.AddInstruction(
            function, *current_dispatch, InstructionOpcode::Equal,
            TypeRef::Builtin(BuiltinType::Bool), std::array{pc, expected}, {},
            std::format("predicate/dispatch/{}/matched", index)),
            "predicate hook dispatch");
        BlockEdge miss;
        if (index + 1 == hooks.size())
            miss = {.target = unknown.id};
        else
        {
            auto arguments = state_arguments(true);
            auto& next = builder.AddBlock(function, arguments);
            miss = {.target = next.id,
                    .arguments = edge_values(receipt, state)};
            dispatch = &next;
        }
        builder.SetTerminator(function, *current_dispatch, {
            .kind = TerminatorKind::ConditionalBranch,
            .condition_or_selector = matched,
            .edges = {{
                .target = hook_blocks.at(hooks[index]->canonical_id),
                .arguments = edge_values(receipt, state),
            }, std::move(miss)},
        }, std::format("predicate/dispatch/{}/branch", index));
    }

    const auto one_u32 = [&](BasicBlock& block, std::string selector)
    {
        return Constant(builder, function, block,
            TypeRef::Builtin(BuiltinType::U32), std::uint32_t{1},
            std::move(selector));
    };
    const auto bool_value = [&](BasicBlock& block, bool value,
                                std::string selector)
    {
        return Constant(builder, function, block,
            TypeRef::Builtin(BuiltinType::Bool), value,
            std::move(selector));
    };
    const auto evaluation_passed = [&](BasicBlock& block,
                                       ProgramValueId evaluation,
                                       const SchemaIdentity& schema,
                                       std::string selector)
    {
        const auto expected = Constant(
            builder, function, block, TypeRef::Named(schema),
            EnumValue{schema, 0}, selector + "/passed");
        return Need(builder.AddInstruction(
            function, block, InstructionOpcode::Equal,
            TypeRef::Builtin(BuiltinType::Bool),
            std::array{evaluation, expected}, {}, selector + "/is-passed"),
            "predicate evaluation comparison");
    };

    const auto lower_result = [&](BasicBlock& block,
                                  ProgramValueId receipt,
                                  std::span<const ProgramValueId> state,
                                  BattleSingleTurnOutcomeV1 outcome,
                                  bool save,
                                  bool capture)
    {
        const auto current_pc = Project(
            builder, function, block, receipt,
            TypeRef::Builtin(BuiltinType::U32), "pc");
        const auto expected_pc = Need(builder.AddInstruction(
            function, block, InstructionOpcode::CheckedConvert,
            TypeRef::Builtin(BuiltinType::U64), std::array{current_pc}, {},
            "result/expected-pc"), "current PC conversion");
        const auto require_request = Construct(
            builder, function, block,
            CanonicalActionInputType(CanonicalAction::ExecutionRequirePausedPc),
            std::array{expected_pc}, "result/paused-request");
        const auto paused = Await(
            builder, function, block,
            CanonicalAction::ExecutionRequirePausedPc, require_request,
            "result/paused");
        const auto vi_end = Project(
            builder, function, block, paused,
            TypeRef::Builtin(BuiltinType::U64), "vi_count");
        const auto rng = ReadRng(
            builder, function, block, receipt, "result/rng");
        if (save)
        {
            (void)Need(builder.AddInstruction(
                function, block, InstructionOpcode::AwaitAction,
                CanonicalActionOutputType(
                    CanonicalAction::SavestateSaveImmutableArtifact),
                std::array{save_request},
                Action(CanonicalAction::SavestateSaveImmutableArtifact),
                "result/save-successor"), "successor savestate");
        }
        ProgramValueId output_context = context;
        if (capture)
        {
            const auto epoch = Project(
                builder, function, block, paused,
                TypeRef::Builtin(BuiltinType::U64), "workset_epoch");
            output_context = CaptureContext(
                builder, function, block, epoch, current_pc,
                "result/context");
        }
        const auto outcome_value = Constant(
            builder, function, block, OutcomeType(),
            EnumValue{OutcomeSchema(), static_cast<std::int64_t>(outcome)},
            "result/outcome");
        const auto has_context = bool_value(
            block, capture, "result/has-context");
        const std::array fields{
            outcome_value, rng, vi_start, vi_end, state[0], state[1],
            cumulative, has_context, output_context};
        const auto result = Construct(
            builder, function, block, ResultType(), fields, "result/value");
        const auto succeeded = bool_value(
            block, true, "result/domain-success");
        builder.SetTerminator(function, block, {
            .kind = TerminatorKind::Return,
            .return_value = result,
            .domain_outcome = succeeded,
        }, "result/return");
    };

    for (const auto* hook : hooks)
    {
        auto block_it = std::ranges::find(
            function.blocks, hook_blocks.at(hook->canonical_id),
            &BasicBlock::id);
        if (block_it == function.blocks.end())
            throw std::logic_error("predicate hook block disappeared");
        BasicBlock* current = &*block_it;
        ProgramValueId receipt = current->arguments[0].id;
        auto state = ids(current->arguments, 1);

        // Binding-owned baselines are captured before any member evaluation at
        // the same hook.  This ordering is part of the execution package.
        for (const auto& baseline : baselines)
        {
            if (baseline.source->baseline_capture_hook_id != hook->canonical_id)
                continue;
            const auto value = AcquireObservation(
                builder, function, *current, receipt, hook->pc,
                *baseline.source,
                std::format("predicate/binding/{}/witness/{}/baseline/capture",
                    baseline.binding_revision_id, baseline.witness_ordinal));
            const auto offset = baseline_state.at(std::pair{
                baseline.binding_revision_id, baseline.witness_ordinal});
            if (baseline.source->baseline_update_policy ==
                predicates::PredicateBaselineUpdatePolicyV1::First)
            {
                state[offset + 1] = Need(builder.AddInstruction(
                    function, *current, InstructionOpcode::Select,
                    baseline.source->value_type,
                    std::array{state[offset], state[offset + 1], value}, {},
                    std::format("predicate/binding/{}/witness/{}/baseline/first",
                        baseline.binding_revision_id,
                        baseline.witness_ordinal)),
                    "predicate baseline selection");
            }
            else
            {
                state[offset + 1] = value;
            }
            state[offset] = bool_value(
                *current, true,
                std::format("predicate/binding/{}/witness/{}/baseline/present",
                    baseline.binding_revision_id, baseline.witness_ordinal));
        }

        for (const auto& lowered : lowered_members)
        {
            const auto& member = *lowered.member;
            const auto& binding = *lowered.binding;
            if (lowered.semantic_hook_id != hook->canonical_id) continue;
            const auto check_offset = check_state.at(member.ordinal);
            const auto one = one_u32(
                *current, std::format("predicate/member/{}/one", member.ordinal));
            const auto next_hit = Need(builder.AddInstruction(
                function, *current, InstructionOpcode::AddChecked,
                TypeRef::Builtin(BuiltinType::U32),
                std::array{state[check_offset], one}, {},
                std::format("predicate/member/{}/hit", member.ordinal)),
                "predicate occurrence accounting");
            state[check_offset] = next_hit;

            ProgramValueId should_evaluate{};
            using Occurrence = predicates::PredicateOccurrencePolicyV1;
            if (member.occurrence == Occurrence::Every)
                should_evaluate = bool_value(
                    *current, true,
                    std::format("predicate/member/{}/every", member.ordinal));
            else if (member.occurrence == Occurrence::Ordinal)
            {
                const auto expected = Constant(
                    builder, function, *current,
                    TypeRef::Builtin(BuiltinType::U32),
                    *member.occurrence_ordinal,
                    std::format("predicate/member/{}/ordinal", member.ordinal));
                should_evaluate = Need(builder.AddInstruction(
                    function, *current, InstructionOpcode::Equal,
                    TypeRef::Builtin(BuiltinType::Bool),
                    std::array{next_hit, expected}, {},
                    std::format("predicate/member/{}/at-ordinal", member.ordinal)),
                    "predicate ordinal comparison");
            }
            else
            {
                should_evaluate = Need(builder.AddInstruction(
                    function, *current, InstructionOpcode::BooleanNot,
                    TypeRef::Builtin(BuiltinType::Bool),
                    std::array{state[check_offset + 1]}, {},
                    std::format("predicate/member/{}/not-complete", member.ordinal)),
                    "predicate completion guard");
            }

            auto evaluation_arguments = state_arguments(true);
            auto& evaluation = builder.AddBlock(function, evaluation_arguments);
            auto merge_arguments = state_arguments(true);
            auto& merge = builder.AddBlock(function, merge_arguments);
            builder.SetTerminator(function, *current, {
                .kind = TerminatorKind::ConditionalBranch,
                .condition_or_selector = should_evaluate,
                .edges = {{
                    .target = evaluation.id,
                    .arguments = edge_values(receipt, state),
                }, {
                    .target = merge.id,
                    .arguments = edge_values(receipt, state),
                }},
            }, std::format("predicate/member/{}/occurrence", member.ordinal));

            BasicBlock* evaluation_block = &evaluation;
            ProgramValueId evaluation_receipt = evaluation.arguments[0].id;
            auto evaluation_state = ids(evaluation.arguments, 1);
            std::vector<ProgramValueId> witnesses;
            witnesses.reserve(binding.witnesses.size());
            for (const auto& witness : binding.witnesses)
            {
                using Source = predicates::PredicateWitnessSourceKindV1;
                if (witness.source_kind == Source::ConcreteValue)
                {
                    witnesses.push_back(Constant(
                        builder, function, *evaluation_block,
                        witness.value_type, witness.concrete_value->payload,
                        std::format("predicate/member/{}/witness/{}/concrete",
                            member.ordinal, witness.witness_ordinal)));
                }
                else if (witness.source_kind == Source::CurrentHookReceipt)
                {
                    witnesses.push_back(HookReceiptField(
                        builder, function, *evaluation_block,
                        evaluation_receipt, hook->pc, witness.value_type,
                        witness.source_field,
                        std::format("predicate/member/{}/witness/{}/receipt",
                            member.ordinal, witness.witness_ordinal)));
                }
                else if (witness.source_kind ==
                             Source::DerivedStateQuery ||
                         witness.source_kind == Source::PinnedGuestMemory)
                {
                    witnesses.push_back(AcquireObservation(
                        builder, function, *evaluation_block,
                        evaluation_receipt, hook->pc,
                        witness,
                        std::format("predicate/member/{}/witness/{}/observation",
                            member.ordinal, witness.witness_ordinal)));
                }
                else
                {
                    const auto offset = baseline_state.at(std::pair{
                        binding.execution_binding_revision_id,
                        witness.witness_ordinal});
                    auto available_arguments = state_arguments(true);
                    auto& available = builder.AddBlock(
                        function, available_arguments);
                    auto& missing = builder.AddBlock(function);
                    builder.SetTerminator(function, *evaluation_block, {
                        .kind = TerminatorKind::ConditionalBranch,
                        .condition_or_selector = evaluation_state[offset],
                        .edges = {{
                            .target = available.id,
                            .arguments = edge_values(
                                evaluation_receipt, evaluation_state),
                        }, {.target = missing.id}},
                    }, std::format("predicate/member/{}/witness/{}/baseline-present",
                        member.ordinal, witness.witness_ordinal));
                    builder.SetTerminator(function, missing, {
                        .kind = TerminatorKind::StructuredFail,
                        .failure = StructuredFailure{
                            "predicate_evidence_missing",
                            "A required predicate baseline was not captured"},
                    }, std::format("predicate/member/{}/witness/{}/baseline-missing",
                        member.ordinal, witness.witness_ordinal));
                    evaluation_block = &available;
                    evaluation_receipt = available.arguments[0].id;
                    evaluation_state = ids(available.arguments, 1);
                    witnesses.push_back(evaluation_state[offset + 1]);
                }
            }

            if (member.occurrence == Occurrence::GuardOnce)
            {
                const auto guard_evaluation = Need(builder.AddInstruction(
                    function, *evaluation_block, InstructionOpcode::CallLocal,
                    TypeRef::Named(*lowered.guard_evaluation_schema),
                    witnesses,
                    {.kind = InstructionTargetKind::LocalFunction,
                     .local_function = *lowered.guard_function},
                    std::format("predicate/member/{}/guard", member.ordinal)),
                    "predicate occurrence guard");
                const auto guard_passed = evaluation_passed(
                    *evaluation_block, guard_evaluation,
                    *lowered.guard_evaluation_schema,
                    std::format("predicate/member/{}/guard", member.ordinal));
                auto actual_arguments = state_arguments(true);
                auto& actual = builder.AddBlock(function, actual_arguments);
                builder.SetTerminator(function, *evaluation_block, {
                    .kind = TerminatorKind::ConditionalBranch,
                    .condition_or_selector = guard_passed,
                    .edges = {{
                        .target = actual.id,
                        .arguments = edge_values(
                            evaluation_receipt, evaluation_state),
                    }, {
                        .target = merge.id,
                        .arguments = edge_values(
                            evaluation_receipt, evaluation_state),
                    }},
                }, std::format("predicate/member/{}/guard-branch", member.ordinal));
                evaluation_block = &actual;
                evaluation_receipt = actual.arguments[0].id;
                evaluation_state = ids(actual.arguments, 1);
            }

            const auto evaluation_value = Need(builder.AddInstruction(
                function, *evaluation_block, InstructionOpcode::CallLocal,
                TypeRef::Named(lowered.evaluation_schema), witnesses,
                {.kind = InstructionTargetKind::LocalFunction,
                 .local_function = lowered.function},
                std::format("predicate/member/{}/evaluate", member.ordinal)),
                "predicate evaluation");
            const auto passed = evaluation_passed(
                *evaluation_block, evaluation_value,
                lowered.evaluation_schema,
                std::format("predicate/member/{}/evaluation", member.ordinal));
            if (member.participates_in_aggregation)
            {
                const auto count_one = one_u32(
                    *evaluation_block,
                    std::format("predicate/member/{}/count-one", member.ordinal));
                const auto count_zero = Constant(
                    builder, function, *evaluation_block,
                    TypeRef::Builtin(BuiltinType::U32), std::uint32_t{0},
                    std::format("predicate/member/{}/count-zero", member.ordinal));
                evaluation_state[1] = Need(builder.AddInstruction(
                    function, *evaluation_block, InstructionOpcode::AddChecked,
                    TypeRef::Builtin(BuiltinType::U32),
                    std::array{evaluation_state[1], count_one}, {},
                    std::format("predicate/member/{}/total", member.ordinal)),
                    "predicate total accounting");
                const auto passed_increment = Need(builder.AddInstruction(
                    function, *evaluation_block, InstructionOpcode::Select,
                    TypeRef::Builtin(BuiltinType::U32),
                    std::array{passed, count_one, count_zero}, {},
                    std::format("predicate/member/{}/passed-increment", member.ordinal)),
                    "predicate passed selection");
                evaluation_state[0] = Need(builder.AddInstruction(
                    function, *evaluation_block, InstructionOpcode::AddChecked,
                    TypeRef::Builtin(BuiltinType::U32),
                    std::array{evaluation_state[0], passed_increment}, {},
                    std::format("predicate/member/{}/passed-total", member.ordinal)),
                    "predicate passed accounting");
            }
            if (member.occurrence != Occurrence::Every)
                evaluation_state[check_offset + 1] = bool_value(
                    *evaluation_block, true,
                    std::format("predicate/member/{}/complete", member.ordinal));

            if (member.reaction == PredicateReaction::AbortOnFail)
            {
                builder.SetTerminator(function, *evaluation_block, {
                    .kind = TerminatorKind::ConditionalBranch,
                    .condition_or_selector = passed,
                    .edges = {{
                        .target = merge.id,
                        .arguments = edge_values(
                            evaluation_receipt, evaluation_state),
                    }, {
                        .target = rejection.id,
                        .arguments = edge_values(
                            evaluation_receipt, evaluation_state),
                    }},
                }, std::format("predicate/member/{}/abort-on-fail", member.ordinal));
            }
            else
            {
                builder.SetTerminator(function, *evaluation_block, {
                    .kind = TerminatorKind::Branch,
                    .edges = {{
                        .target = merge.id,
                        .arguments = edge_values(
                            evaluation_receipt, evaluation_state),
                    }},
                }, std::format("predicate/member/{}/continue", member.ordinal));
            }
            current = &merge;
            receipt = merge.arguments[0].id;
            state = ids(merge.arguments, 1);
        }

        if (hook->pc == TurnInputsPc)
            lower_result(*current, receipt, state,
                BattleSingleTurnOutcomeV1::ReachedNextTurn, true, true);
        else if (hook->pc == VictoryPc)
            lower_result(*current, receipt, state,
                BattleSingleTurnOutcomeV1::Victory, true, false);
        else if (hook->pc == DefeatPc)
            lower_result(*current, receipt, state,
                BattleSingleTurnOutcomeV1::Defeat, false, false);
        else
            builder.SetTerminator(function, *current, {
                .kind = TerminatorKind::Branch,
                .edges = {{.target = continuation.id, .arguments = state}},
            }, std::format("predicate/hook/{}/continue", hook->canonical_id));
    }

    {
        const auto state = ids(continuation.arguments);
        const auto next_receipt = ContinueTo(
            builder, function, continuation, semantic_points,
            "predicate/continue");
        builder.SetTerminator(function, continuation, {
            .kind = TerminatorKind::Branch,
            .edges = {{
                .target = loop.id,
                .arguments = edge_values(next_receipt, state),
            }},
        }, "predicate/continue/loop");
    }
    if (has_abort_on_fail)
    {
        const auto receipt = rejection.arguments[0].id;
        const auto state = ids(rejection.arguments, 1);
        lower_result(rejection, receipt, state,
            BattleSingleTurnOutcomeV1::PredicateRejected, false, false);
    }
    else
    {
        builder.SetTerminator(function, rejection, {
            .kind = TerminatorKind::StructuredFail,
            .failure = StructuredFailure{
                "predicate_rejection_unreachable",
                "No active AbortOnFail predicate can reach rejection"},
        }, "predicate/rejection-unreachable");
    }
}

ProgramModule ConstructModule(
    bool first_turn,
    const predicates::PredicateExecutionPackageV1& predicate_package){
  ProgramModule module{
      .identity = {.canonical_id = std::string(ModuleCanonicalId) +
                                   (first_turn ? ".first" : ".later"),
                   .revision = 3}};
  const auto lowered_members = LowerActivePredicates(predicate_package, module);
  const auto interaction = LowerInteraction(CommandInteraction(), module);
  if (!interaction || !interaction.function)
    throw std::logic_error("battle command interaction lowering failed");
  Builder b(module, "BattleSingleTurnModule", "battle.single_turn/execute/v3");
  b.AddLocalType(
      {.identity = RequestSchema(),
       .kind = TypeSchemaKind::Record,
       .record_fields = {
           {"turn_index", TypeRef::Builtin(BuiltinType::U32)},
           {"cumulative_fake_attacks_before",
            TypeRef::Builtin(BuiltinType::U32)},
           {"plan", TypeRef::Named(
                        capabilities::BattleTurnExecutionSpecSchemaIdentity())},
           {"confirmed_seed_frame",
            CanonicalRuntimeType(CanonicalRuntimeSchema::InputFramePayload)},
           {"save_request",
            CanonicalActionInputType(
                CanonicalAction::SavestateSaveImmutableArtifact)}}});
  b.AddLocalType({.identity = OutcomeSchema(),
                  .kind = TypeSchemaKind::ClosedEnum,
                  .enum_members = OutcomeMembers()});
  b.AddLocalType(
      {.identity = ResultSchema(),
       .kind = TypeSchemaKind::Record,
       .record_fields = {
           {"outcome", OutcomeType()},
           {"ending_rng", TypeRef::Builtin(BuiltinType::U32)},
           {"vi_start", TypeRef::Builtin(BuiltinType::U64)},
           {"vi_end", TypeRef::Builtin(BuiltinType::U64)},
           {"pred_passed", TypeRef::Builtin(BuiltinType::U32)},
           {"pred_total", TypeRef::Builtin(BuiltinType::U32)},
           {"cumulative_fake_attacks", TypeRef::Builtin(BuiltinType::U32)},
           {"has_context", TypeRef::Builtin(BuiltinType::Bool)},
           {"context",
            TypeRef::Named(capabilities::BattleContextSchemaIdentity())}}});
  b.AddCapabilityImport(capabilities::BattlePackIdentity());
  b.AddCapabilityImport(capabilities::BattleCommandPackIdentity());
  b.AddTypeImport(capabilities::BattleContextSchemaIdentity());
  b.AddTypeImport(capabilities::BattleTurnExecutionSpecSchemaIdentity());
  b.AddTypeImport(capabilities::BattleDerivedFreshnessSchemaIdentity());
  b.AddTypeImport(capabilities::BattleDerivedQueryRequestSchemaIdentity());
  b.AddActionImport(capabilities::BattleCaptureContextActionIdentity());
  b.AddReducerImport(CanonicalReducerIdentity(
      CanonicalReducer::BattlePrepareCommandInteraction));
  for (auto a :
       {CanonicalAction::ExecutionRequirePausedPc,
        CanonicalAction::ExecutionContinueUntil,
        CanonicalAction::ExecutionStepFrames, CanonicalAction::GuestReadU8,
        CanonicalAction::GuestReadU16, CanonicalAction::GuestReadU32,
        CanonicalAction::GuestReadU64,
        CanonicalAction::SavestateSaveImmutableArtifact,
        CanonicalAction::InputAcquireLease, CanonicalAction::InputApplyState,
        CanonicalAction::InputBeginDelivery,
        CanonicalAction::InputCompleteDelivery})
    AddAction(b, a);
  const auto savestate_artifact_schema =
      CanonicalActionArtifactPayloadSchemaIdentity(
          CanonicalAction::SavestateSaveImmutableArtifact);
  if (!savestate_artifact_schema)
    throw std::logic_error("savestate artifact schema is unavailable");
  b.AddTypeImport(*savestate_artifact_schema);
  b.AddTypeImport(capabilities::BattleCaptureContextRequestSchemaIdentity());
  b.AddTypeImport(capabilities::BattleCommandStateSchemaIdentity());
  b.AddTypeImport(capabilities::BattleCommandPreparationSchemaIdentity());
  const auto arg = b.NewArgument(RequestType());
  auto &f =
      b.AddFunction(std::string(Entrypoint), std::array{arg}, ResultType(),
                    TypeRef::Builtin(BuiltinType::Bool), true);
  f.blocks.reserve(4096);
  auto &entry = b.AddBlock(f);
  const auto entry_receipt = RequirePc(
      b, f, entry, first_turn ? BeforeRandSeedSetPc : TurnInputsPc, "entry");
  const auto vi_start = Project(b, f, entry, entry_receipt,
                                TypeRef::Builtin(BuiltinType::U64), "vi_count");
  const auto turn_index = Project(
      b, f, entry, arg.id, TypeRef::Builtin(BuiltinType::U32), "turn_index");
  const auto cumulative_before =
      Project(b, f, entry, arg.id, TypeRef::Builtin(BuiltinType::U32),
              "cumulative_fake_attacks_before");
  const auto plan = Project(
      b, f, entry, arg.id,
      TypeRef::Named(capabilities::BattleTurnExecutionSpecSchemaIdentity()),
      "plan");
  const auto save_request = Project(
      b, f, entry, arg.id,
      CanonicalActionInputType(CanonicalAction::SavestateSaveImmutableArtifact),
      "save_request");
  ProgramValueId turn_stop = entry_receipt;
  if (first_turn) {
    const auto frame =
        Project(b, f, entry, arg.id,
                CanonicalRuntimeType(CanonicalRuntimeSchema::InputFramePayload),
                "confirmed_seed_frame");
    (void)ApplyConfirmedSeedFrame(b, f, entry, frame);
    const std::array turn_points{Point("TurnInputs", TurnInputsPc)};
    turn_stop = ContinueTo(b, f, entry, turn_points, "prelude/turn-inputs");
  }
    const auto epoch=Project(b,f,entry,turn_stop,TypeRef::Builtin(BuiltinType::U64),"workset_epoch");const auto pc=Project(b,f,entry,turn_stop,TypeRef::Builtin(BuiltinType::U32),"pc");const auto context=CaptureContext(b,f,entry,epoch,pc,"turn-inputs/context");const auto prepared=Need(b.AddInstruction(f,entry,InstructionOpcode::CallReducer,TypeRef::Named(capabilities::BattleCommandPreparationSchemaIdentity()),std::array{context,plan},Reducer(CanonicalReducer::BattlePrepareCommandInteraction),"plan/validate-and-prepare-adaptive-interaction"),"battle command preparation");const auto prepared_ok=Project(b,f,entry,prepared,TypeRef::Builtin(BuiltinType::Bool),"success");const auto interaction_state=Project(b,f,entry,prepared,TypeRef::Named(capabilities::BattleCommandStateSchemaIdentity()),"state");auto& execute=b.AddBlock(f);auto& invalid=b.AddBlock(f);b.SetTerminator(f,entry,{.kind=TerminatorKind::ConditionalBranch,.condition_or_selector=prepared_ok,.edges={{.target=execute.id},{.target=invalid.id}}},"plan/dispatch");b.SetTerminator(f,invalid,{.kind=TerminatorKind::StructuredFail,.failure=StructuredFailure{"battle_plan_invalid","Battle Plan is inconsistent with the live Battle Context"}},"plan/fail");
    LowerTurnExecution(b,f,execute,*interaction.function,interaction_state,context,plan,cumulative_before,vi_start,save_request,predicate_package,lowered_members);
  module.accepted_policies={.state_policies={InvocationStatePolicy::RestoreBaseline},.execution_intents={ExecutionIntent::Live}};module.budgets={.maximum_instructions=kBattleInstructionBudget,.maximum_calls=kBattleCallBudget,.maximum_call_depth=8,.maximum_action_requests=kBattleActionBudget,.maximum_emissions=256,.maximum_artifacts=1,.maximum_values=kBattleValueBudget,.maximum_value_bytes=16*1024*1024,.maximum_trace_events=kBattleTraceBudget};std::set<SchemaIdentity> emission_schema_set;for(const auto& lowered:lowered_members)if(lowered.member->emit_evidence||lowered.member->reaction==PredicateReaction::AbortOnFail)emission_schema_set.insert(lowered.evaluation_schema);std::vector<SchemaIdentity> emission_schemas(emission_schema_set.begin(),emission_schema_set.end());module.entrypoints={{.name=std::string(Entrypoint),.function=f.id,.input_type=RequestType(),.output_type=ResultType(),.domain_outcome_type=TypeRef::Builtin(BuiltinType::Bool),.emission_schemas=std::move(emission_schemas),.artifact_schemas={*savestate_artifact_schema},.required_capability_packs=module.required_capability_packs,.accepted_policies=module.accepted_policies}};module.identity.module_hash=ComputeProgramModuleHashV1(module);return module;
}

RuntimeProfile Profile(const ProgramDependencyLock& dependencies){return {.profile_id="soa-usa-jit64-v1",.game_id=std::string(capabilities::kSupportedGameId),.disc_identity=std::string(capabilities::kSupportedGameId),.executable_identity=std::string(capabilities::kSupportedExecutableIdentity),.backend="jit64",.capability_packs=dependencies.capability_packs};}
std::string ProfileHash(const RuntimeProfile&p){std::string c=p.profile_id+'\0'+p.game_id+'\0'+p.disc_identity+'\0'+p.executable_identity+'\0'+p.backend;return hash::sha256(c.data(),c.size());}
std::optional<ProgramDependencyLock> Verify(const ProgramModule&m,std::string*d){ProgramDefinitionStore modules;TypeSchemaRegistry schemas;ActionRegistry actions(&schemas);CapabilityPackRegistry packs(&schemas,&actions);auto registered=capabilities::RegisterSourceCapabilityPacks(schemas,actions,packs);if(!registered.success){SetDiagnostic(d,registered.error.message);return std::nullopt;}auto stored=modules.RegisterCompiled(m);if(!stored.success){SetDiagnostic(d,stored.error.message);return std::nullopt;}ProgramVerifier verifier(modules,schemas,actions,packs);auto verified=verifier.Verify(stored.module->identity,capabilities::SupportedSoaUsaCompatibility());if(!verified.success||!verified.verified){std::string diagnostic="battle.single_turn verification failed";for(const auto& item:verified.diagnostics){diagnostic+="; "+item.message;if(item.source_location){diagnostic+=" @source="+std::to_string(item.source_location->value());const auto source=std::ranges::find(m.source_map.entries,*item.source_location,&SourceMapEntry::id);if(source!=m.source_map.entries.end())diagnostic+="("+source->semantic_path+")";}}SetDiagnostic(d,std::move(diagnostic));return std::nullopt;}return verified.verified->dependency_lock;}

class GraphAssembler{public:ProgramValueId Scalar(TypeRef type,ProgramValuePayload payload){const auto id=ProgramValueId(next++);values.push_back({id,std::move(type),std::move(payload)});return id;}ProgramValueId Import(const ProgramValueGraph& graph){std::map<std::uint64_t,ProgramValueId> ids;for(const auto&v:graph.values)ids.emplace(v.id.value(),ProgramValueId(next++));for(const auto&v:graph.values){auto copy=v;copy.id=ids.at(v.id.value());std::visit([&](auto& payload){using T=std::decay_t<decltype(payload)>;if constexpr(std::is_same_v<T,RecordValue>)for(auto&id:payload.fields)id=ids.at(id.value());else if constexpr(std::is_same_v<T,ListValue>)for(auto&id:payload.elements)id=ids.at(id.value());else if constexpr(std::is_same_v<T,OptionalValue>)if(payload.value)payload.value=ids.at(payload.value->value());},copy.payload);values.push_back(std::move(copy));}return ids.at(graph.root.value());}ProgramValueGraph Finish(ProgramValueId root){return {root,std::move(values)};}private:std::uint64_t next=1;std::vector<ProgramValue>values;};
ProgramValueGraph InputGraph(const BattleSingleTurnRequestV1&r,std::string*d){GraphAssembler g;const auto turn=g.Scalar(TypeRef::Builtin(BuiltinType::U32),r.turn_index);const auto cumulative=g.Scalar(TypeRef::Builtin(BuiltinType::U32),r.cumulative_fake_attacks_before);const auto plan=g.Import(capabilities::EncodeBattleTurnExecutionSpecValue(r.plan));const auto frame=g.Scalar(CanonicalRuntimeType(CanonicalRuntimeSchema::InputFramePayload),FrameBytes(r.confirmed_seed_frame.value_or(GCInputFrame{})));CanonicalActionPayload payload;if(!payload.AddUtf8(CanonicalActionPayloadField::Path,r.output_savestate_path)||!payload.AddUtf8(CanonicalActionPayloadField::Label,"battle.single_turn successor")){SetDiagnostic(d,"savestate request could not be encoded");return {};}auto encoded=EncodeCanonicalActionPayload(payload,*CanonicalActionInputType(CanonicalAction::SavestateSaveImmutableArtifact).named);if(!encoded.ok){SetDiagnostic(d,encoded.diagnostic);return {};}const auto save=g.Import(encoded.graph);const auto root=g.Scalar(RequestType(),RecordValue{.fields={turn,cumulative,plan,frame,save}});return g.Finish(root);}

const ProgramValue* Find(const ProgramValueGraph&g,ProgramValueId id){const auto found=std::ranges::find(g.values,id,&ProgramValue::id);return found==g.values.end()?nullptr:&*found;}
bool DecodeOutput(const ProgramValueGraph&g,BattleSingleTurnResultV1&out){const auto*root=Find(g,g.root);const auto*record=root?std::get_if<RecordValue>(&root->payload):nullptr;if(!record||record->fields.size()!=9)return false;const auto scalar=[&](std::size_t i,auto&v){const auto*x=Find(g,record->fields[i]);using T=std::remove_reference_t<decltype(v)>;const auto*p=x?std::get_if<T>(&x->payload):nullptr;if(!p)return false;v=*p;return true;};const auto*oe=Find(g,record->fields[0]);const auto*e=oe?std::get_if<EnumValue>(&oe->payload):nullptr;const auto*outcome=e?FindBattleSingleTurnOutcomeDefinitionV1(e->value):nullptr;if(outcome==nullptr)return false;out.outcome=outcome->outcome;if(!scalar(1,out.ending_rng)||!scalar(2,out.vi_start)||!scalar(3,out.vi_end)||!scalar(4,out.pred_passed)||!scalar(5,out.pred_total)||!scalar(6,out.cumulative_fake_attacks)||!scalar(7,out.has_battle_context))return false;ProgramValueGraph context=g;context.root=record->fields[8];return capabilities::DecodeBattleContextValue(context,out.battle_context);}

class Definition final:public IBattleSingleTurnFullPhaseDefinitionV1{public:Definition(bool first,predicates::PredicateExecutionPackageV1 predicate_package):first_(first),predicate_package_(std::move(predicate_package)){std::string diagnostic;module_=ConstructModule(first_,predicate_package_);auto dependencies=Verify(module_,&diagnostic);auto encoded=EncodeProgramModuleV1(module_);if(!dependencies||!encoded)throw std::logic_error(diagnostic.empty()?encoded.status.message:diagnostic);dependencies_=*dependencies;profile_=Profile(*dependencies);envelope_={{module_.identity.canonical_id,module_.identity.revision,module_.identity.module_hash.ToHex()},kProgramCodecVersionV1,false,encoded.bytes};InvocationExecutionPolicy execution{.intent=ExecutionIntent::Live,.allow_input=true,.record_trace=false};runtime_={.module=envelope_.identity,.entrypoint=std::string(Entrypoint),.dependency_lock_sha256=ComputeProgramDependencyLockHashV1(*dependencies).ToHex(),.runtime_profile_sha256=ProfileHash(profile_),.state_policy=InvocationStatePolicy::RestoreBaseline,.execution=execution,.limits=module_.budgets,.baseline_lineage=first_?"soa.battle.single_turn/prebattle-entry/v1":"soa.battle.single_turn/turn-inputs/v1"};ProgramInvocation sample=Resolve({.turn_index=first_?1u:2u,.plan={.commands={{.actor_slot=0,.macro=soa::battle::actions::BattleAction::Attack,.params={.target_slot=4}}}},.confirmed_seed_frame=first_?std::optional<GCInputFrame>(GCInputFrame{}):std::nullopt,.output_savestate_path="successor.sav"},ProgramExecutionId(1),AttemptId(1),nullptr);runtime_.verified_dependency_sha256=ComputeProgramInvocationCompatibilityHashV1(sample);constexpr std::string_view movie="soa.battle.single_turn/no-movie/v1";runtime_.movie_policy_sha256=hash::sha256(movie.data(),movie.size());std::string service="soa.battle.single_turn/exact-entry-interaction/v1\0"+predicate_package_.content_sha256;runtime_.service_policy_sha256=hash::sha256(service.data(),service.size());std::string canonical=std::string(FullPhaseCanonicalId)+'\0'+runtime_.module.canonical_hash+'\0'+predicate_package_.content_sha256+'\0'+(first_?'1':'0');identity_={static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner),ProgramVersion,std::string(FullPhaseCanonicalId),1,hash::sha256(canonical.data(),canonical.size())};}
    const fullphase::FullPhaseProgramIdentity&identity()const noexcept override{return identity_;}const fullphase::FullPhaseRuntimeContract&runtime_contract()const noexcept override{return runtime_;}const EncodedModuleEnvelope&module_envelope()const noexcept override{return envelope_;}const predicates::PredicateExecutionPackageV1&predicate_package()const noexcept override{return predicate_package_;}bool first_turn()const noexcept override{return first_;}
    std::optional<ProgramInvocation>BuildResolvedExecution(std::span<const std::uint8_t>input,ProgramExecutionId execution,AttemptId attempt,std::string*d)const override{BattleSingleTurnRequestV1 request;if(!DecodeBattleSingleTurnExecutionInputV1(input,request,d)||request.turn_index==0||first_!=(request.turn_index==1)||first_!=request.confirmed_seed_frame.has_value()||request.output_savestate_path.empty())return std::nullopt;return Resolve(request,execution,attempt,d);}
    bool DecodeProgramResult(std::span<const Byte>bytes,BattleSingleTurnResultV1&out,std::string*d)const override{auto decoded=DecodeProgramResultV1(bytes);if(!decoded||!decoded.value||decoded.value->module!=module_.identity||decoded.value->resolved_dependencies!=dependencies_||decoded.value->infrastructure!=ProgramInfrastructureStatus::Completed||decoded.value->cleanup!=ProgramCleanupStatus::Clean||decoded.value->session_disposition!=SessionDisposition::Clean||!decoded.value->output){SetDiagnostic(d,"battle.single_turn did not complete cleanly");return false;}if(!DecodeOutput(*decoded.value->output,out)){SetDiagnostic(d,"battle.single_turn result is malformed");return false;}out.predicate_group_revision_id=predicate_package_.group.predicate_group_revision_id;out.predicate_group_sha256=predicate_package_.group.content_sha256;out.predicate_execution_package_sha256=predicate_package_.content_sha256;out.predicate_evidence=decoded.value->emissions;out.artifacts=decoded.value->artifacts;return true;}
 private:ProgramInvocation Resolve(const BattleSingleTurnRequestV1&r,ProgramExecutionId execution,AttemptId attempt,std::string*d)const{auto input=InputGraph(r,d);return {.invocation_id=execution,.attempt_id=attempt,.module=module_.identity,.entrypoint=std::string(Entrypoint),.dependencies=dependencies_,.runtime_profile=profile_,.state={.policy=InvocationStatePolicy::RestoreBaseline,.session_lineage=runtime_.baseline_lineage},.execution=runtime_.execution,.input=std::move(input),.limits=runtime_.limits,.provenance={.requesting_component="SavorDb.battle.single_turn",.attributes={{"predicate_execution_package",predicate_package_.content_sha256}}}};}
    bool first_{};predicates::PredicateExecutionPackageV1 predicate_package_;ProgramModule module_;ProgramDependencyLock dependencies_;RuntimeProfile profile_;EncodedModuleEnvelope envelope_;fullphase::FullPhaseRuntimeContract runtime_;fullphase::FullPhaseProgramIdentity identity_;
};

class KindHandler final:public fullphase::IFullPhaseProgramDefinition{public:KindHandler(){base_=std::make_shared<Definition>(false,predicates::EmptyPredicateExecutionPackageV1());}const fullphase::FullPhaseProgramIdentity&identity()const noexcept override{return base_->identity();}const fullphase::FullPhaseRuntimeContract&runtime_contract()const noexcept override{return base_->runtime_contract();}const EncodedModuleEnvelope&module_envelope()const noexcept override{return base_->module_envelope();}std::optional<ProgramInvocation>BuildResolvedExecution(std::span<const std::uint8_t>,ProgramExecutionId,AttemptId,std::string*d)const override{SetDiagnostic(d,"battle.single_turn requires a prepared workset package");return std::nullopt;}std::optional<ProgramInvocation>BuildResolvedExecution(const fullphase::FullPhaseProgramPackage&package,std::span<const std::uint8_t>common,std::span<const std::uint8_t>item,ProgramExecutionId execution,AttemptId attempt,std::string*d)const override{predicates::PredicateExecutionPackageV1 predicate_package;bool first=false;if(common.empty()||common.front()>1||!predicates::DecodePredicateExecutionPackageV1(common.subspan(1),predicate_package,d)){SetDiagnostic(d,"battle.single_turn common input is invalid");return std::nullopt;}first=common.front()!=0;auto prepared=PrepareBattleSingleTurnFullPhaseV1(first,std::move(predicate_package),d);if(!prepared||fullphase::BuildFullPhaseProgramPackage(*prepared)!=package){SetDiagnostic(d,"battle.single_turn prepared package identity drifted");return std::nullopt;}return prepared->BuildResolvedExecution(item,execution,attempt,d);}private:std::shared_ptr<const Definition>base_;};

} // namespace

program::composition::InteractionDefinition BattleCommandInteractionV3()
{
    return CommandInteraction();
}

std::vector<std::uint8_t> EncodeBattleSingleTurnCommonInputV1(
    bool first_turn,
    const predicates::PredicateExecutionPackageV1& predicate_package)
{
    std::vector<std::uint8_t> encoded_package;
    std::string diagnostic;
    if (!predicates::EncodePredicateExecutionPackageV1(
            predicate_package, encoded_package, &diagnostic))
    {
        return {};
    }
    std::vector<std::uint8_t> output;
    output.reserve(encoded_package.size() + 1);
    output.push_back(first_turn ? 1u : 0u);
    output.insert(output.end(), encoded_package.begin(), encoded_package.end());
    return output;
}

std::vector<std::uint8_t> EncodeBattleSingleTurnExecutionInputV1(const BattleSingleTurnRequestV1&r){Writer w;for(auto b:kWireMagic)w.U8(b);w.U32(r.turn_index);w.U32(r.cumulative_fake_attacks_before);w.U32(r.plan.fake_attack_count);std::vector<std::uint8_t> commands;soa::battle::actions::encode_battle_turn_commands_to_buffer(r.plan.commands,commands);w.U32(static_cast<std::uint32_t>(commands.size()));for(auto b:commands)w.U8(b);w.U8(r.confirmed_seed_frame?1:0);if(r.confirmed_seed_frame)for(auto b:FrameBytes(*r.confirmed_seed_frame))w.U8(b);w.Text(r.output_savestate_path);return std::move(w.bytes);}
bool DecodeBattleSingleTurnExecutionInputV1(std::span<const std::uint8_t>input,BattleSingleTurnRequestV1&r,std::string*d){Reader reader(input);for(auto expected:kWireMagic){std::uint8_t value=0;if(!reader.U8(value)||value!=expected){SetDiagnostic(d,"battle.single_turn input magic is invalid");return false;}}BattleSingleTurnRequestV1 out;std::uint32_t command_size=0;if(!reader.U32(out.turn_index)||!reader.U32(out.cumulative_fake_attacks_before)||!reader.U32(out.plan.fake_attack_count)||!reader.U32(command_size)||command_size>1024){SetDiagnostic(d,"battle.single_turn input header is invalid");return false;}std::vector<std::uint8_t> commands(command_size);for(auto&b:commands)if(!reader.U8(b))return false;if(!soa::battle::actions::decode_battle_turn_commands_from_buffer(commands,out.plan.commands))return false;std::uint8_t has=0;if(!reader.U8(has)||has>1)return false;if(has){GCInputFrame frame;if(!ReadFrame(reader,frame))return false;out.confirmed_seed_frame=frame;}if(!reader.Text(out.output_savestate_path)||!reader.Done()){SetDiagnostic(d,"battle.single_turn input is malformed");return false;}r=std::move(out);return true;}

std::shared_ptr<const IBattleSingleTurnFullPhaseDefinitionV1> PrepareBattleSingleTurnFullPhaseV1(bool first,predicates::PredicateExecutionPackageV1 predicate_package,std::string*d){const auto validation=predicates::ValidatePredicateExecutionPackageV1(predicate_package);if(!validation){SetDiagnostic(d,validation.code+": "+validation.message);return {};}static std::mutex mutex;static std::map<std::pair<bool,std::string>,std::weak_ptr<const Definition>> cache;const auto key=std::pair<bool,std::string>{first,predicate_package.content_sha256};std::lock_guard lock(mutex);if(auto found=cache.find(key);found!=cache.end())if(auto value=found->second.lock())return value;try{auto value=std::make_shared<Definition>(first,std::move(predicate_package));cache[key]=value;return value;}catch(const std::exception&e){SetDiagnostic(d,e.what());return {};}}

std::shared_ptr<const fullphase::IFullPhaseProgramDefinition> BattleSingleTurnKindHandlerV1(){static auto value=std::make_shared<KindHandler>();return value;}

} // namespace savor::runtime::battlesingleturn
