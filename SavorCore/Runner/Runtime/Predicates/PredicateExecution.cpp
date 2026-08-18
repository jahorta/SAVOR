#include "PredicateExecution.h"

#include "../../Breakpoints/BpRegistry.h"
#include "../ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "../ProgramRuntime/Codec/ProgramCodecV1.h"
#include "../ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "../../../Utils/Hash.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace savor::runtime::predicates {
namespace {

using namespace program;
using namespace program::composition;

constexpr std::size_t kMaximumString = 64 * 1024;
constexpr std::size_t kMaximumBlob = 1024 * 1024;
constexpr std::size_t kMaximumEntries = 4096;
constexpr std::string_view kEmptyGroupId = "savor.predicate_group.empty";

void SetDiagnostic(std::string* output, std::string value)
{
    if (output) *output = std::move(value);
}

class Writer
{
public:
    void U8(std::uint8_t v) { bytes.push_back(v); }
    void U32(std::uint32_t v) { for (int s=0;s<32;s+=8) bytes.push_back(static_cast<std::uint8_t>(v>>s)); }
    void U64(std::uint64_t v) { for (int s=0;s<64;s+=8) bytes.push_back(static_cast<std::uint8_t>(v>>s)); }
    void I64(std::int64_t v) { U64(std::bit_cast<std::uint64_t>(v)); }
    void String(std::string_view v) { U32(static_cast<std::uint32_t>(v.size())); bytes.insert(bytes.end(),v.begin(),v.end()); }
    void Blob(std::span<const std::uint8_t> v) { U32(static_cast<std::uint32_t>(v.size())); bytes.insert(bytes.end(),v.begin(),v.end()); }
    std::vector<std::uint8_t> bytes;
};

class Reader
{
public:
    explicit Reader(std::span<const std::uint8_t> source):source_(source){}
    bool U8(std::uint8_t& v){if(offset_>=source_.size())return false;v=source_[offset_++];return true;}
    bool U32(std::uint32_t& v){if(source_.size()-offset_<4)return false;v=0;for(int s=0;s<32;s+=8)v|=static_cast<std::uint32_t>(source_[offset_++])<<s;return true;}
    bool U64(std::uint64_t& v){if(source_.size()-offset_<8)return false;v=0;for(int s=0;s<64;s+=8)v|=static_cast<std::uint64_t>(source_[offset_++])<<s;return true;}
    bool I64(std::int64_t& v){std::uint64_t raw=0;if(!U64(raw))return false;v=std::bit_cast<std::int64_t>(raw);return true;}
    bool String(std::string& v){std::uint32_t n=0;if(!U32(n)||n>kMaximumString||source_.size()-offset_<n)return false;v.assign(reinterpret_cast<const char*>(source_.data()+offset_),n);offset_+=n;return true;}
    bool Blob(std::vector<std::uint8_t>& v){std::uint32_t n=0;if(!U32(n)||n>kMaximumBlob||source_.size()-offset_<n)return false;v.assign(source_.begin()+offset_,source_.begin()+offset_+n);offset_+=n;return true;}
    bool Count(std::uint32_t& v){return U32(v)&&v<=kMaximumEntries;}
    bool Done()const{return offset_==source_.size();}
private:
    std::span<const std::uint8_t> source_;std::size_t offset_=0;
};

void EncodeType(Writer& w,const TypeRef& t){w.U8(t.is_named()?1:0);if(!t.is_named()){w.U8(static_cast<std::uint8_t>(t.builtin));return;}w.String(t.named->canonical_id);w.U32(t.named->version);w.String(t.named->schema_hash.ToHex());}
bool DecodeType(Reader&r,TypeRef&t){std::uint8_t named=0;if(!r.U8(named)||named>1)return false;if(!named){std::uint8_t b=0;if(!r.U8(b)||b>static_cast<std::uint8_t>(BuiltinType::F64))return false;t=TypeRef::Builtin(static_cast<BuiltinType>(b));return true;}std::string id,h;std::uint32_t v=0;if(!r.String(id)||!r.U32(v)||!r.String(h))return false;auto parsed=ContentHash256::FromHex(h);if(id.empty()||v==0||!parsed)return false;t=TypeRef::Named({id,v,*parsed});return true;}
void EncodeDependency(Writer&w,const ExactDependencyIdentity&d){w.String(d.canonical_id);w.U32(d.version);w.String(d.signature_hash.ToHex());}
bool DecodeDependency(Reader&r,ExactDependencyIdentity&d){std::string h;if(!r.String(d.canonical_id)||!r.U32(d.version)||!r.String(h))return false;auto parsed=ContentHash256::FromHex(h);if(d.canonical_id.empty()||d.version==0||!parsed)return false;d.signature_hash=*parsed;return true;}

void EncodeLiteral(Writer&w,const LiteralValue&v)
{
    EncodeType(w,v.type);w.U8(static_cast<std::uint8_t>(v.payload.index()));
    std::visit([&](const auto&p){using T=std::decay_t<decltype(p)>;
        if constexpr(std::is_same_v<T,UnitValue>){}
        else if constexpr(std::is_same_v<T,bool>)w.U8(p?1:0);
        else if constexpr(std::is_same_v<T,std::uint8_t>)w.U8(p);
        else if constexpr(std::is_same_v<T,std::uint16_t>)w.U32(p);
        else if constexpr(std::is_same_v<T,std::uint32_t>)w.U32(p);
        else if constexpr(std::is_same_v<T,std::uint64_t>)w.U64(p);
        else if constexpr(std::is_same_v<T,std::int32_t>||std::is_same_v<T,std::int64_t>)w.I64(p);
        else if constexpr(std::is_same_v<T,float>)w.U32(std::bit_cast<std::uint32_t>(p));
        else if constexpr(std::is_same_v<T,double>)w.U64(std::bit_cast<std::uint64_t>(p));
        else if constexpr(std::is_same_v<T,std::string>)w.String(p);
        else if constexpr(std::is_same_v<T,std::vector<Byte>>)w.Blob(p);
        else if constexpr(std::is_same_v<T,EnumValue>){w.String(p.schema.canonical_id);w.U32(p.schema.version);w.String(p.schema.schema_hash.ToHex());w.I64(p.value);}
    },v.payload);
}

bool DecodeLiteral(Reader&r,LiteralValue&v)
{
    if(!DecodeType(r,v.type))return false;std::uint8_t k=0;if(!r.U8(k)||k>=std::variant_size_v<LiteralPayload>)return false;
    switch(k){
    case 0:v.payload=UnitValue{};return true;case 1:{std::uint8_t x=0;if(!r.U8(x)||x>1)return false;v.payload=x!=0;return true;}
    case 2:{std::uint8_t x=0;if(!r.U8(x))return false;v.payload=x;return true;}case 3:{std::uint32_t x=0;if(!r.U32(x)||x>65535)return false;v.payload=static_cast<std::uint16_t>(x);return true;}
    case 4:{std::uint32_t x=0;if(!r.U32(x))return false;v.payload=x;return true;}case 5:{std::uint64_t x=0;if(!r.U64(x))return false;v.payload=x;return true;}
    case 6:{std::int64_t x=0;if(!r.I64(x)||x<(std::numeric_limits<std::int32_t>::min)()||x>(std::numeric_limits<std::int32_t>::max)())return false;v.payload=static_cast<std::int32_t>(x);return true;}
    case 7:{std::int64_t x=0;if(!r.I64(x))return false;v.payload=x;return true;}case 8:{std::uint32_t x=0;if(!r.U32(x))return false;v.payload=std::bit_cast<float>(x);return true;}
    case 9:{std::uint64_t x=0;if(!r.U64(x))return false;v.payload=std::bit_cast<double>(x);return true;}case 10:{std::string x;if(!r.String(x))return false;v.payload=std::move(x);return true;}
    case 11:{std::vector<std::uint8_t>x;if(!r.Blob(x))return false;v.payload=std::move(x);return true;}case 12:{EnumValue x;std::string h;std::int64_t n=0;if(!r.String(x.schema.canonical_id)||!r.U32(x.schema.version)||!r.String(h)||!r.I64(n))return false;auto parsed=ContentHash256::FromHex(h);if(!parsed)return false;x.schema.schema_hash=*parsed;x.value=n;v.payload=std::move(x);return true;}
    default:return false;}
}

void EncodeDefinitionBody(Writer&w,const PredicateDefinition&d)
{
    w.String(d.canonical_id);w.U32(d.revision);w.String(d.source_name);w.U32(static_cast<std::uint32_t>(d.witnesses.size()));for(const auto&x:d.witnesses){w.String(x.name);EncodeType(w,x.value_type);}w.U32(static_cast<std::uint32_t>(d.expression.size()));
    for(const auto&n:d.expression){w.U8(static_cast<std::uint8_t>(n.kind));w.U8(n.witness_index?1:0);if(n.witness_index)w.U32(static_cast<std::uint32_t>(*n.witness_index));w.U8(n.literal?1:0);if(n.literal)EncodeLiteral(w,*n.literal);w.U32(static_cast<std::uint32_t>(n.operands.size()));for(auto x:n.operands)w.U32(static_cast<std::uint32_t>(x));w.U8(n.reducer?1:0);if(n.reducer)EncodeDependency(w,*n.reducer);EncodeType(w,n.result_type);w.String(n.source_label);}w.U32(static_cast<std::uint32_t>(d.root_expression));
}

bool DecodeDefinitionBody(Reader&r,PredicateDefinition&d)
{
    std::uint32_t n=0;if(!r.String(d.canonical_id)||!r.U32(d.revision)||!r.String(d.source_name)||!r.Count(n))return false;d.witnesses.resize(n);for(auto&x:d.witnesses)if(!r.String(x.name)||!DecodeType(r,x.value_type))return false;if(!r.Count(n))return false;d.expression.resize(n);
    for(auto&x:d.expression){std::uint8_t k=0,p=0;std::uint32_t u=0;if(!r.U8(k)||k>static_cast<std::uint8_t>(PredicateExpressionKind::ImportedReducer)||!r.U8(p)||p>1)return false;x.kind=static_cast<PredicateExpressionKind>(k);if(p){if(!r.U32(u))return false;x.witness_index=u;}if(!r.U8(p)||p>1)return false;if(p){LiteralValue v;if(!DecodeLiteral(r,v))return false;x.literal=std::move(v);}if(!r.Count(n))return false;x.operands.resize(n);for(auto&o:x.operands){if(!r.U32(u))return false;o=u;}if(!r.U8(p)||p>1)return false;if(p){ExactDependencyIdentity dep;if(!DecodeDependency(r,dep))return false;x.reducer=std::move(dep);}if(!DecodeType(r,x.result_type)||!r.String(x.source_label))return false;}if(!r.U32(n))return false;d.root_expression=n;return true;
}

void EncodeHook(Writer&w,const PredicateHookContractV1&v,bool hash){w.String(v.canonical_id);w.U32(v.revision);w.String(hash?v.content_sha256:std::string_view{});w.U32(static_cast<std::uint32_t>(v.points.size()));for(const auto&p:v.points){w.String(p.canonical_id);w.U32(p.pc);}}
bool DecodeHook(Reader&r,PredicateHookContractV1&v){std::uint32_t n=0;if(!r.String(v.canonical_id)||!r.U32(v.revision)||!r.String(v.content_sha256)||!r.Count(n))return false;v.points.resize(n);for(auto&p:v.points)if(!r.String(p.canonical_id)||!r.U32(p.pc))return false;return true;}

void EncodeSource(Writer&w,const PredicateWitnessSourceBindingV1&v)
{
    w.U32(v.witness_ordinal);w.U8(static_cast<std::uint8_t>(v.source_kind));EncodeType(w,v.value_type);w.U8(v.concrete_value?1:0);if(v.concrete_value)EncodeLiteral(w,*v.concrete_value);w.U8(v.source?1:0);if(v.source)EncodeDependency(w,*v.source);w.U8(static_cast<std::uint8_t>(v.observation_source_kind));w.String(v.source_field);w.U8(v.pinned_guest_address?1:0);if(v.pinned_guest_address)w.U64(*v.pinned_guest_address);w.String(v.baseline_capture_hook_id);w.U8(static_cast<std::uint8_t>(v.baseline_update_policy));
}
bool DecodeSource(Reader&r,PredicateWitnessSourceBindingV1&v)
{
    std::uint8_t k=0,p=0;std::uint64_t a=0;if(!r.U32(v.witness_ordinal)||!r.U8(k)||k>4||!DecodeType(r,v.value_type)||!r.U8(p)||p>1)return false;v.source_kind=static_cast<PredicateWitnessSourceKindV1>(k);if(p){LiteralValue x;if(!DecodeLiteral(r,x))return false;v.concrete_value=std::move(x);}if(!r.U8(p)||p>1)return false;if(p){ExactDependencyIdentity d;if(!DecodeDependency(r,d))return false;v.source=std::move(d);}if(!r.U8(k)||k>3)return false;v.observation_source_kind=static_cast<PredicateObservationSourceKindV1>(k);if(!r.String(v.source_field)||!r.U8(p)||p>1||(p&&!r.U64(a))||!r.String(v.baseline_capture_hook_id)||!r.U8(k)||k>1)return false;if(p)v.pinned_guest_address=a;v.baseline_update_policy=static_cast<PredicateBaselineUpdatePolicyV1>(k);return true;
}

void EncodeBinding(Writer&w,const PredicateExecutionBindingV1&v,bool hash)
{
    w.I64(v.execution_binding_revision_id);w.String(v.canonical_id);w.U32(v.revision);w.String(hash?v.content_sha256:std::string_view{});w.I64(v.definition.revision_id);w.String(v.definition.content_sha256);EncodeDefinitionBody(w,v.definition.definition);w.U32(static_cast<std::uint32_t>(v.witnesses.size()));for(const auto&x:v.witnesses)EncodeSource(w,x);
}
bool DecodeBinding(Reader&r,PredicateExecutionBindingV1&v)
{
    std::uint32_t n=0;if(!r.I64(v.execution_binding_revision_id)||v.execution_binding_revision_id<=0||!r.String(v.canonical_id)||!r.U32(v.revision)||!r.String(v.content_sha256)||!r.I64(v.definition.revision_id)||v.definition.revision_id<=0||!r.String(v.definition.content_sha256)||!DecodeDefinitionBody(r,v.definition.definition)||!r.Count(n))return false;v.witnesses.resize(n);for(auto&x:v.witnesses)if(!DecodeSource(r,x))return false;return true;
}

void EncodeGroup(Writer&w,const ResolvedPredicateGroupV1&v,bool hash)
{
    w.I64(v.predicate_group_revision_id);w.String(v.canonical_id);w.U32(v.revision);w.String(hash?v.content_sha256:std::string_view{});w.U32(static_cast<std::uint32_t>(v.members.size()));for(const auto&m:v.members){w.U32(m.ordinal);w.I64(m.execution_binding_revision_id);w.U32(static_cast<std::uint32_t>(m.semantic_hook_ids.size()));for(const auto&h:m.semantic_hook_ids)w.String(h);w.U8(static_cast<std::uint8_t>(m.occurrence));w.U8(m.occurrence_ordinal?1:0);if(m.occurrence_ordinal)w.U32(*m.occurrence_ordinal);w.U8(m.guard_execution_binding_revision_id?1:0);if(m.guard_execution_binding_revision_id)w.I64(*m.guard_execution_binding_revision_id);w.U8(static_cast<std::uint8_t>(m.reaction));w.U8(m.participates_in_aggregation?1:0);w.U8(m.emit_evidence?1:0);}
}
bool DecodeGroup(Reader&r,ResolvedPredicateGroupV1&v)
{
    std::uint32_t n=0;std::uint8_t k=0,p=0;if(!r.I64(v.predicate_group_revision_id)||v.predicate_group_revision_id<=0||!r.String(v.canonical_id)||!r.U32(v.revision)||!r.String(v.content_sha256)||!r.Count(n))return false;v.members.resize(n);for(auto&m:v.members){if(!r.U32(m.ordinal)||!r.I64(m.execution_binding_revision_id)||m.execution_binding_revision_id<=0||!r.Count(n))return false;m.semantic_hook_ids.resize(n);for(auto&h:m.semantic_hook_ids)if(!r.String(h))return false;if(!r.U8(k)||k>3||!r.U8(p)||p>1)return false;m.occurrence=static_cast<PredicateOccurrencePolicyV1>(k);if(p){std::uint32_t o=0;if(!r.U32(o))return false;m.occurrence_ordinal=o;}if(!r.U8(p)||p>1)return false;if(p){std::int64_t g=0;if(!r.I64(g)||g<=0)return false;m.guard_execution_binding_revision_id=g;}if(!r.U8(k)||k>1||!r.U8(p)||p>1)return false;m.reaction=static_cast<PredicateReaction>(k);m.participates_in_aggregation=p!=0;if(!r.U8(p)||p>1)return false;m.emit_evidence=p!=0;}return true;
}

std::string Hash(std::span<const std::uint8_t>b){return hash::sha256(b.data(),b.size());}
PredicateExecutionPackageValidationResult Invalid(std::string c,std::string m){return{false,std::move(c),std::move(m)};}

bool ReceiptFieldValid(std::string_view field,const TypeRef&type)
{
    return (field=="pc"&&type==TypeRef::Builtin(BuiltinType::U32))||((field=="movie_input_count"||field=="workset_epoch"||field=="vi_count")&&type==TypeRef::Builtin(BuiltinType::U64));
}

std::optional<CanonicalAction> GuestReadFor(const TypeRef&type)
{
    if(type.is_named())return std::nullopt;switch(type.builtin){case BuiltinType::U8:return CanonicalAction::GuestReadU8;case BuiltinType::U16:return CanonicalAction::GuestReadU16;case BuiltinType::U32:return CanonicalAction::GuestReadU32;case BuiltinType::U64:return CanonicalAction::GuestReadU64;default:return std::nullopt;}
}

const PredicateExecutionBindingV1* FindBinding(const PredicateExecutionPackageV1&p,std::int64_t id)
{
    const auto found=std::ranges::find(p.execution_bindings,id,&PredicateExecutionBindingV1::execution_binding_revision_id);return found==p.execution_bindings.end()?nullptr:&*found;
}

bool SourceSchedulableAt(const PredicateAuthoringCatalogV2&catalog,const PredicateWitnessSourceBindingV1&source,std::string_view hook)
{
    if(source.source_kind==PredicateWitnessSourceKindV1::ConcreteValue||source.source_kind==PredicateWitnessSourceKindV1::CurrentHookReceipt||source.source_kind==PredicateWitnessSourceKindV1::PinnedGuestMemory)return true;
    const auto& list=source.observation_source_kind==PredicateObservationSourceKindV1::RegisteredReducer?catalog.reducers:catalog.query_sources;
    const auto found=source.source?std::ranges::find(list,*source.source,&PredicateAuthoringSourceV1::identity):list.end();
    if(found==list.end())return false;
    if(source.source_kind!=PredicateWitnessSourceKindV1::DerivedStateQuery)return true;
    return std::ranges::find(found->evaluation_hook_ids,hook)!=found->evaluation_hook_ids.end();
}

} // namespace

PredicateHookContractV1 BattlePredicateHookContractV1()
{
    PredicateHookContractV1 value{.canonical_id="soa.battle.predicate_hooks",.revision=2};
    for(const auto&point:bp::BpRegistry::ForConsumer(BreakpointConsumer::Predicate)){if(bp::domain_of(point.key)!=bp::BPDomain::Battle||point.key==bp::battle::StartAction)continue;value.points.push_back({point.stable_id,point.pc});}
    std::ranges::sort(value.points,{},&PredicateHookPointV1::canonical_id);value.content_sha256=ComputePredicateHookContractHashV1(value);return value;
}

PredicateAuthoringCatalogV2 BattlePredicateAuthoringCatalogV2()
{
    PredicateAuthoringCatalogV2 value{.canonical_id="soa.battle.predicate_authoring_catalog",.revision=4,.hook_contract=BattlePredicateHookContractV1()};
    const auto source=capabilities::BuildSourceCapabilityPackCatalog();
    const auto require_hook=[&](BPKey key,std::string display,std::string description){const auto*point=bp::BpRegistry::FindRuntime(key);if(!point||!point->stable_id)throw std::logic_error("Battle predicate hook catalog is incomplete");value.hooks.push_back({point->stable_id,std::move(display),std::move(description),point->pc});return std::string(point->stable_id);};
    const auto turn_inputs=require_hook(bp::battle::TurnInputs,"Turn inputs","The Battle is ready to accept commands for the current turn.");
    const auto turn_ready=require_hook(bp::battle::TurnIsReady,"Turn is ready","The complete action order for the turn is available.");
    const auto end_turn=require_hook(bp::battle::EndTurn,"End turn","A non-victory turn has committed its rewards.");
    const auto victory=require_hook(bp::battle::EndBattleVictory,"Victory","The victory path has committed its rewards before End Turn.");
    const auto snapshot=TypeRef::Named(capabilities::BattleDerivedSnapshotSchemaIdentity());
    const auto turn_entry_query=capabilities::BattleDerivedTurnEntryActionIdentity();
    const auto turn_order_query=capabilities::BattleDerivedTurnOrderActionIdentity();
    const auto rewards_query=capabilities::BattleDerivedRewardsActionIdentity();
    value.semantic_inputs={
        {"battle.turn_inputs","turn_inputs","Turn inputs","Current turn and usable inventory at command entry.",snapshot,turn_entry_query},
        {"battle.turn_order","turn_order","Turn order","The ordered active player and enemy participants.",snapshot,turn_order_query},
        {"battle.rewards","rewards","Rewards","Cumulative item drops and current Battle turn.",snapshot,rewards_query},
    };
    for(const auto&a:source.actions){std::vector<std::string> capture,evaluation;std::string display,category,description;if(a.identity==turn_entry_query){capture={turn_inputs};evaluation={turn_inputs,turn_ready,end_turn,victory};display="Current turn inputs";category="Turn inputs";description="Capture the current turn and usable inventory when command entry is reached.";}else if(a.identity==turn_order_query){capture={turn_ready};evaluation={turn_ready,end_turn,victory};display="Current turn order";category="Turn order";description="Capture the active participant order when the turn order is committed.";}else if(a.identity==rewards_query){capture={end_turn,victory};evaluation={end_turn,victory};display="Current rewards";category="Rewards";description="Capture cumulative Battle rewards on either terminal path.";}else continue;std::ranges::sort(capture);std::ranges::sort(evaluation);value.query_sources.push_back({.display_name=std::move(display),.category=std::move(category),.description=std::move(description),.source_kind=PredicateObservationSourceKindV1::RegisteredQuery,.identity=a.identity,.result_type=a.output_type,.capture_hook_ids=std::move(capture),.evaluation_hook_ids=std::move(evaluation)});}
    const auto friendly_reducer=[](std::string_view id){if(id.ends_with("current_turn"))return std::pair{"Battle","Current battle turn"};if(id.ends_with("inventory_count"))return std::pair{"Turn inputs","Usable item count"};if(id.ends_with("drop_count"))return std::pair{"Rewards","Cumulative item drop count"};if(id.ends_with("player_count"))return std::pair{"Turn order","Active player count"};if(id.ends_with("enemy_count"))return std::pair{"Turn order","Active enemy count"};if(id.ends_with("player_min_position"))return std::pair{"Turn order","First player turn position"};if(id.ends_with("player_max_position"))return std::pair{"Turn order","Last player turn position"};if(id.ends_with("enemy_min_position"))return std::pair{"Turn order","First enemy turn position"};return std::pair{"Turn order","Last enemy turn position"};};
    for(const auto&r:source.reducers){if(!r.identity.canonical_id.starts_with("soa.battle.derived."))continue;const auto [category,display]=friendly_reducer(r.identity.canonical_id);value.reducers.push_back({.display_name=display,.category=category,.description="A typed Battle derived-state value.",.source_kind=PredicateObservationSourceKindV1::RegisteredReducer,.identity=r.identity,.argument_types=r.input_types,.result_type=r.output_type});}
    value.hook_receipt_fields={{"pc","Hook PC","The guest program counter for the routed hook.",TypeRef::Builtin(BuiltinType::U32)},{"movie_input_count","Movie input count","The movie cursor at the routed hook.",TypeRef::Builtin(BuiltinType::U64)},{"workset_epoch","Workset epoch","The active item epoch.",TypeRef::Builtin(BuiltinType::U64)},{"vi_count","VI count","The video-interface count at the routed hook.",TypeRef::Builtin(BuiltinType::U64)}};
    const auto reducer=[&](std::string_view id)->ExactDependencyIdentity{const auto found=std::ranges::find(value.reducers,id,[](const auto&v){return std::string_view(v.identity.canonical_id);});if(found==value.reducers.end())throw std::logic_error("Battle predicate reducer is unavailable");return found->identity;};
    const TypeRef u16=TypeRef::Builtin(BuiltinType::U16),u32=TypeRef::Builtin(BuiltinType::U32);
    const auto automatic=[](std::string key,std::string label,std::string role,TypeRef type){return PredicateAuthoringValueArgumentV2{std::move(key),std::move(label),"Provided by the selected Battle data family.",std::move(type),std::move(role),std::nullopt};};
    const auto parameter=[](std::string key,std::string label,std::string suggested,TypeRef type){return PredicateAuthoringValueArgumentV2{std::move(key),std::move(label),"A reusable Predicate Definition parameter supplied by an Execution Binding.",std::move(type),std::nullopt,std::move(suggested)};};
    const auto recipe=[&](std::string id,std::string category,std::string display,std::string description,std::string_view reducer_id,std::vector<PredicateAuthoringValueArgumentV2>arguments){value.value_recipes.push_back({std::move(id),std::move(category),std::move(display),std::move(description),reducer(reducer_id),std::move(arguments),u32});};
    recipe("battle.current_turn","Battle","Current battle turn","The current Battle turn. Its authoritative snapshot is captured automatically.","soa.battle.derived.current_turn",{automatic("snapshot","Battle state","battle.turn_inputs",snapshot)});
    recipe("battle.turn_inputs.inventory_count","Turn inputs","Usable item count","The usable inventory count for an item.","soa.battle.derived.inventory_count",{automatic("snapshot","Turn inputs","battle.turn_inputs",snapshot),parameter("item_id","Item ID","item_id",u16)});
    recipe("battle.turn_order.player_count","Turn order","Active player count","The number of active player participants.","soa.battle.derived.player_count",{automatic("snapshot","Turn order","battle.turn_order",snapshot)});
    recipe("battle.turn_order.enemy_count","Turn order","Active enemy count","The number of active enemy participants.","soa.battle.derived.enemy_count",{automatic("snapshot","Turn order","battle.turn_order",snapshot)});
    recipe("battle.turn_order.player_min_position","Turn order","First player turn position","The earliest player position in the ordered turn.","soa.battle.derived.player_min_position",{automatic("snapshot","Turn order","battle.turn_order",snapshot)});
    recipe("battle.turn_order.player_max_position","Turn order","Last player turn position","The latest player position in the ordered turn.","soa.battle.derived.player_max_position",{automatic("snapshot","Turn order","battle.turn_order",snapshot)});
    recipe("battle.turn_order.enemy_min_position","Turn order","First enemy turn position","The earliest enemy position in the ordered turn.","soa.battle.derived.enemy_min_position",{automatic("snapshot","Turn order","battle.turn_order",snapshot)});
    recipe("battle.turn_order.enemy_max_position","Turn order","Last enemy turn position","The latest enemy position in the ordered turn.","soa.battle.derived.enemy_max_position",{automatic("snapshot","Turn order","battle.turn_order",snapshot)});
    recipe("battle.rewards.drop_count","Rewards","Cumulative item drop count","The cumulative drop count for an item.","soa.battle.derived.drop_count",{automatic("snapshot","Rewards","battle.rewards",snapshot),parameter("item_id","Item ID","item_id",u16)});
    value.comparison_operators={
        {"equals","Equals","Both values must be equal.",PredicateGuidedNodeKindV1::Equal,false},
        {"not_equal","Does not equal","The values must be different.",PredicateGuidedNodeKindV1::NotEqual,false},
        {"less","Is before / less than","The left value must occur before or be smaller than the right value.",PredicateGuidedNodeKindV1::Less,true},
        {"less_equal","Is at most","The left value must be no greater than the right value.",PredicateGuidedNodeKindV1::LessEqual,true},
        {"greater","Is after / greater than","The left value must occur after or be larger than the right value.",PredicateGuidedNodeKindV1::Greater,true},
        {"greater_equal","Is at least","The left value must be no smaller than the right value.",PredicateGuidedNodeKindV1::GreaterEqual,true},
    };
    value.calculation_operators={
        {"add","Add","Add the two values.",PredicateGuidedNodeKindV1::Add,true},
        {"subtract","Subtract","Subtract the right value from the left value.",PredicateGuidedNodeKindV1::Subtract,true},
        {"multiply","Multiply","Multiply the two values.",PredicateGuidedNodeKindV1::Multiply,true},
        {"divide","Divide","Divide the left value by the right value.",PredicateGuidedNodeKindV1::Divide,true},
        {"remainder","Remainder","Use the remainder after integer division.",PredicateGuidedNodeKindV1::Remainder,true},
    };
    for(int i=static_cast<int>(BuiltinType::Unit);i<=static_cast<int>(BuiltinType::F64);++i)value.value_types.push_back(TypeRef::Builtin(static_cast<BuiltinType>(i)));value.value_types.push_back(TypeRef::Named(capabilities::BattleDerivedSnapshotSchemaIdentity()));
    std::ranges::sort(value.hooks,{},&PredicateAuthoringHookV2::canonical_id);std::ranges::sort(value.query_sources,{},[](const auto&x){return x.identity.canonical_id;});std::ranges::sort(value.reducers,{},[](const auto&x){return x.identity.canonical_id;});std::ranges::sort(value.semantic_inputs,{},&PredicateAuthoringSemanticInputV2::role_id);std::ranges::sort(value.value_recipes,{},&PredicateAuthoringValueRecipeV2::recipe_id);std::ranges::sort(value.comparison_operators,{},&PredicateAuthoringOperatorV2::operator_id);std::ranges::sort(value.calculation_operators,{},&PredicateAuthoringOperatorV2::operator_id);value.content_sha256=ComputePredicateAuthoringCatalogHashV2(value);return value;
}

namespace {

bool GuidedNumeric(const TypeRef&type)
{
    if(type.is_named())return false;
    switch(type.builtin){case BuiltinType::U8:case BuiltinType::U16:case BuiltinType::U32:case BuiltinType::U64:case BuiltinType::I32:case BuiltinType::I64:return true;default:return false;}
}

struct GuidedValueRef{std::size_t expression=0;TypeRef type=TypeRef::Builtin(BuiltinType::Unit);};

class GuidedCompiler
{
public:
    GuidedCompiler(const PredicateAuthoringCatalogV2&catalog):catalog_(catalog)
    {
        definition_.canonical_id="guided.predicate";definition_.revision=1;definition_.source_name="Savor.PredicateAuthoring";
    }

    PredicateGuidedCompileResultV1 Compile(const PredicateGuidedNodeV1&root)
    {
        const auto compiled=Node(root,"root",std::nullopt);
        if(compiled&&compiled->type!=TypeRef::Builtin(BuiltinType::Bool))Error("root","predicate.non_boolean_root","The predicate rule must produce true or false.");
        if(!compiled||!diagnostics_.empty())return{false,{},std::move(diagnostics_)};
        definition_.root_expression=compiled->expression;
        ProgramModule module;PredicateEvaluationPolicy policy{.predicate_group_revision_id=1,.execution_binding_revision_id=1,.semantic_point_id="authoring.validation"};
        const auto lowered=LowerPredicate(definition_,policy,module);
        if(!lowered){for(const auto&d:lowered.diagnostics)Error("root",d.code,d.message);return{false,{},std::move(diagnostics_)};}
        return{true,std::move(definition_),{}};
    }

private:
    void Error(std::string path,std::string code,std::string message){diagnostics_.push_back({std::move(path),std::move(code),std::move(message)});}
    const PredicateAuthoringSemanticInputV2* Input(std::string_view role)const{const auto found=std::ranges::find(catalog_.semantic_inputs,role,&PredicateAuthoringSemanticInputV2::role_id);return found==catalog_.semantic_inputs.end()?nullptr:&*found;}
    const PredicateAuthoringValueRecipeV2* Recipe(std::string_view id)const{const auto found=std::ranges::find(catalog_.value_recipes,id,&PredicateAuthoringValueRecipeV2::recipe_id);return found==catalog_.value_recipes.end()?nullptr:&*found;}
    std::optional<GuidedValueRef> Witness(std::string name,const TypeRef&type,std::string label,std::string_view path)
    {
        const auto found=witnesses_.find(name);std::size_t ordinal=0;
        if(found!=witnesses_.end()){ordinal=found->second;if(definition_.witnesses[ordinal].value_type!=type){Error(std::string(path),"predicate.parameter_type_conflict","The same input name is used with incompatible types.");return std::nullopt;}}
        else{ordinal=definition_.witnesses.size();witnesses_.emplace(name,ordinal);definition_.witnesses.push_back({std::move(name),type});}
        const auto node=witness_nodes_.find(ordinal);if(node!=witness_nodes_.end())return GuidedValueRef{node->second,type};
        const auto index=definition_.expression.size();definition_.expression.push_back({.kind=PredicateExpressionKind::Witness,.witness_index=ordinal,.result_type=type,.source_label=std::move(label)});witness_nodes_.emplace(ordinal,index);return GuidedValueRef{index,type};
    }
    std::optional<GuidedValueRef> Node(const PredicateGuidedNodeV1&node,std::string path,std::optional<TypeRef>expected)
    {
        const auto child_path=[&](std::size_t index){return path+"/"+std::to_string(index);};
        if(node.kind==PredicateGuidedNodeKindV1::SemanticInput){const auto*input=Input(node.key);if(!input){Error(path,"predicate.semantic_input_unavailable","The selected semantic input is no longer available.");return std::nullopt;}if(expected&&*expected!=input->value_type){Error(path,"predicate.type_mismatch","The semantic input has the wrong type for this value.");return std::nullopt;}return Witness(input->witness_name,input->value_type,input->display_name,path);}
        if(node.kind==PredicateGuidedNodeKindV1::Parameter){if(node.key.empty()){Error(path,"predicate.parameter_name_required","A parameter name is required.");return std::nullopt;}const auto type=node.declared_type?node.declared_type:expected;if(!type){Error(path,"predicate.parameter_type_required","The parameter type cannot be inferred here.");return std::nullopt;}if(expected&&*type!=*expected){Error(path,"predicate.type_mismatch","The parameter type is incompatible with this value.");return std::nullopt;}return Witness(node.key,*type,node.key,path);}
        if(node.kind==PredicateGuidedNodeKindV1::Literal){if(!node.literal){Error(path,"predicate.literal_required","A typed literal value is required.");return std::nullopt;}if(expected&&node.literal->type!=*expected){Error(path,"predicate.type_mismatch","The literal type is incompatible with this value.");return std::nullopt;}const auto index=definition_.expression.size();definition_.expression.push_back({.kind=PredicateExpressionKind::Literal,.literal=node.literal,.result_type=node.literal->type,.source_label="literal"});return GuidedValueRef{index,node.literal->type};}
        if(node.kind==PredicateGuidedNodeKindV1::CatalogValue){const auto*recipe=Recipe(node.key);if(!recipe){Error(path,"predicate.recipe_unavailable","The selected Battle value is no longer in the authoring catalog.");return std::nullopt;}std::vector<std::size_t>operands;std::size_t visible=0;for(std::size_t i=0;i<recipe->arguments.size();++i){const auto&a=recipe->arguments[i];std::optional<GuidedValueRef>argument;if(a.automatic_semantic_input_role){PredicateGuidedNodeV1 automatic{.kind=PredicateGuidedNodeKindV1::SemanticInput,.key=*a.automatic_semantic_input_role};argument=Node(automatic,path+"/"+a.key,a.value_type);}else if(visible<node.children.size())argument=Node(node.children[visible++],path+"/"+a.key,a.value_type);else{Error(path+"/"+a.key,"predicate.argument_required","A value is required for "+a.display_name+".");return std::nullopt;}if(!argument)return std::nullopt;operands.push_back(argument->expression);}if(visible!=node.children.size()){Error(path,"predicate.invalid_arity","The selected Battle value has unexpected arguments.");return std::nullopt;}if(expected&&recipe->result_type!=*expected){Error(path,"predicate.type_mismatch","The selected Battle value has the wrong result type.");return std::nullopt;}const auto index=definition_.expression.size();definition_.expression.push_back({.kind=PredicateExpressionKind::ImportedReducer,.operands=std::move(operands),.reducer=recipe->reducer,.result_type=recipe->result_type,.source_label=recipe->display_name});return GuidedValueRef{index,recipe->result_type};}
        const bool comparison=node.kind>=PredicateGuidedNodeKindV1::Equal&&node.kind<=PredicateGuidedNodeKindV1::GreaterEqual;
        const bool arithmetic=node.kind>=PredicateGuidedNodeKindV1::Add&&node.kind<=PredicateGuidedNodeKindV1::Remainder;
        if(node.kind==PredicateGuidedNodeKindV1::All||node.kind==PredicateGuidedNodeKindV1::Any){if(node.children.size()<2){Error(path,"predicate.invalid_arity","All and Any groups require at least two conditions.");return std::nullopt;}auto current=Node(node.children[0],child_path(0),TypeRef::Builtin(BuiltinType::Bool));if(!current)return std::nullopt;for(std::size_t i=1;i<node.children.size();++i){const auto next=Node(node.children[i],child_path(i),TypeRef::Builtin(BuiltinType::Bool));if(!next)return std::nullopt;const auto index=definition_.expression.size();definition_.expression.push_back({.kind=node.kind==PredicateGuidedNodeKindV1::All?PredicateExpressionKind::BooleanAnd:PredicateExpressionKind::BooleanOr,.operands={current->expression,next->expression},.result_type=TypeRef::Builtin(BuiltinType::Bool),.source_label=node.kind==PredicateGuidedNodeKindV1::All?"all conditions":"any condition"});current=GuidedValueRef{index,TypeRef::Builtin(BuiltinType::Bool)};}return current;}
        if(node.kind==PredicateGuidedNodeKindV1::Not){if(node.children.size()!=1){Error(path,"predicate.invalid_arity","Not requires exactly one condition.");return std::nullopt;}const auto child=Node(node.children.front(),child_path(0),TypeRef::Builtin(BuiltinType::Bool));if(!child)return std::nullopt;const auto index=definition_.expression.size();definition_.expression.push_back({.kind=PredicateExpressionKind::BooleanNot,.operands={child->expression},.result_type=TypeRef::Builtin(BuiltinType::Bool),.source_label="not"});return GuidedValueRef{index,TypeRef::Builtin(BuiltinType::Bool)};}
        if(comparison||arithmetic){if(node.children.size()!=2){Error(path,"predicate.invalid_arity","This operator requires a left and right value.");return std::nullopt;}const auto left=Node(node.children[0],child_path(0),std::nullopt);if(!left)return std::nullopt;const auto right=Node(node.children[1],child_path(1),left->type);if(!right)return std::nullopt;if(left->type!=right->type){Error(path,"predicate.type_mismatch","The left and right values must have the same type.");return std::nullopt;}if(arithmetic&&!GuidedNumeric(left->type)){Error(path,"predicate.type_mismatch","Arithmetic requires integer values.");return std::nullopt;}PredicateExpressionKind kind=PredicateExpressionKind::Equal;switch(node.kind){case PredicateGuidedNodeKindV1::Equal:kind=PredicateExpressionKind::Equal;break;case PredicateGuidedNodeKindV1::NotEqual:kind=PredicateExpressionKind::NotEqual;break;case PredicateGuidedNodeKindV1::Less:kind=PredicateExpressionKind::Less;break;case PredicateGuidedNodeKindV1::LessEqual:kind=PredicateExpressionKind::LessEqual;break;case PredicateGuidedNodeKindV1::Greater:kind=PredicateExpressionKind::Greater;break;case PredicateGuidedNodeKindV1::GreaterEqual:kind=PredicateExpressionKind::GreaterEqual;break;case PredicateGuidedNodeKindV1::Add:kind=PredicateExpressionKind::Add;break;case PredicateGuidedNodeKindV1::Subtract:kind=PredicateExpressionKind::Subtract;break;case PredicateGuidedNodeKindV1::Multiply:kind=PredicateExpressionKind::Multiply;break;case PredicateGuidedNodeKindV1::Divide:kind=PredicateExpressionKind::Divide;break;case PredicateGuidedNodeKindV1::Remainder:kind=PredicateExpressionKind::Remainder;break;default:break;}const auto type=comparison?TypeRef::Builtin(BuiltinType::Bool):left->type;if(expected&&type!=*expected){Error(path,"predicate.type_mismatch","The expression result has the wrong type here.");return std::nullopt;}const auto index=definition_.expression.size();definition_.expression.push_back({.kind=kind,.operands={left->expression,right->expression},.result_type=type,.source_label=comparison?"comparison":"arithmetic"});return GuidedValueRef{index,type};}
        Error(path,"predicate.guided_kind_invalid","The guided expression kind is invalid.");return std::nullopt;
    }
    const PredicateAuthoringCatalogV2&catalog_;PredicateDefinition definition_;std::map<std::string,std::size_t>witnesses_;std::map<std::size_t,std::size_t>witness_nodes_;std::vector<PredicateAuthoringDiagnosticV1>diagnostics_;
};

PredicateGuidedNodeKindV1 GuidedKind(PredicateExpressionKind kind)
{
    switch(kind){case PredicateExpressionKind::Equal:return PredicateGuidedNodeKindV1::Equal;case PredicateExpressionKind::NotEqual:return PredicateGuidedNodeKindV1::NotEqual;case PredicateExpressionKind::Less:return PredicateGuidedNodeKindV1::Less;case PredicateExpressionKind::LessEqual:return PredicateGuidedNodeKindV1::LessEqual;case PredicateExpressionKind::Greater:return PredicateGuidedNodeKindV1::Greater;case PredicateExpressionKind::GreaterEqual:return PredicateGuidedNodeKindV1::GreaterEqual;case PredicateExpressionKind::BooleanAnd:return PredicateGuidedNodeKindV1::All;case PredicateExpressionKind::BooleanOr:return PredicateGuidedNodeKindV1::Any;case PredicateExpressionKind::BooleanNot:return PredicateGuidedNodeKindV1::Not;case PredicateExpressionKind::Add:return PredicateGuidedNodeKindV1::Add;case PredicateExpressionKind::Subtract:return PredicateGuidedNodeKindV1::Subtract;case PredicateExpressionKind::Multiply:return PredicateGuidedNodeKindV1::Multiply;case PredicateExpressionKind::Divide:return PredicateGuidedNodeKindV1::Divide;case PredicateExpressionKind::Remainder:return PredicateGuidedNodeKindV1::Remainder;default:return PredicateGuidedNodeKindV1::Literal;}
}

class GuidedReconstructor
{
public:
    GuidedReconstructor(const PredicateDefinition&definition,const PredicateAuthoringCatalogV2&catalog):definition_(definition),catalog_(catalog){}
    PredicateGuidedReconstructionResultV1 Run(){if(definition_.expression.empty()||definition_.root_expression>=definition_.expression.size())return{false,{},{{"root","predicate.invalid_expression","The stored Predicate Definition has no valid root."}}};const auto root=Node(definition_.root_expression,"root");return root&&diagnostics_.empty()?PredicateGuidedReconstructionResultV1{true,*root,{}}:PredicateGuidedReconstructionResultV1{false,root.value_or(PredicateGuidedNodeV1{}),std::move(diagnostics_)};}
private:
    void Error(std::string path,std::string code,std::string message){diagnostics_.push_back({std::move(path),std::move(code),std::move(message)});}
    const PredicateAuthoringSemanticInputV2* Semantic(const PredicateWitness&w)const{const auto found=std::ranges::find_if(catalog_.semantic_inputs,[&](const auto&i){return i.witness_name==w.name&&i.value_type==w.value_type;});return found==catalog_.semantic_inputs.end()?nullptr:&*found;}
    const PredicateAuthoringValueRecipeV2* Recipe(const PredicateExpressionNode&node)const
    {
        if(!node.reducer)return nullptr;
        for(const auto&r:catalog_.value_recipes){if(r.reducer!=*node.reducer||r.arguments.size()!=node.operands.size())continue;bool matches=true;for(std::size_t i=0;i<r.arguments.size();++i){const auto&a=r.arguments[i];if(!a.automatic_semantic_input_role)continue;const auto input=std::ranges::find(catalog_.semantic_inputs,*a.automatic_semantic_input_role,&PredicateAuthoringSemanticInputV2::role_id);const auto&operand=definition_.expression[node.operands[i]];matches=matches&&input!=catalog_.semantic_inputs.end()&&operand.kind==PredicateExpressionKind::Witness&&operand.witness_index&&definition_.witnesses[*operand.witness_index].name==input->witness_name;}if(matches)return&r;}return nullptr;
    }
    std::optional<PredicateGuidedNodeV1> Node(std::size_t index,std::string path)
    {
        if(index>=definition_.expression.size()){Error(path,"predicate.invalid_expression","An expression dependency is missing.");return std::nullopt;}if(active_.contains(index)){Error(path,"predicate.expression_cycle","The expression contains a cycle.");return std::nullopt;}active_.insert(index);const auto&node=definition_.expression[index];PredicateGuidedNodeV1 result;
        if(node.kind==PredicateExpressionKind::Witness){if(!node.witness_index||*node.witness_index>=definition_.witnesses.size()){Error(path,"predicate.invalid_witness","A stored witness reference is invalid.");active_.erase(index);return std::nullopt;}const auto&w=definition_.witnesses[*node.witness_index];if(const auto*semantic=Semantic(w)){result.kind=PredicateGuidedNodeKindV1::SemanticInput;result.key=semantic->role_id;}else{result.kind=PredicateGuidedNodeKindV1::Parameter;result.key=w.name;result.declared_type=w.value_type;}}
        else if(node.kind==PredicateExpressionKind::Literal){result.kind=PredicateGuidedNodeKindV1::Literal;result.literal=node.literal;result.declared_type=node.result_type;}
        else if(node.kind==PredicateExpressionKind::ImportedReducer){const auto*recipe=Recipe(node);result.kind=PredicateGuidedNodeKindV1::CatalogValue;if(!recipe){result.key=node.reducer?node.reducer->canonical_id:"unavailable";Error(path,"predicate.recipe_unavailable","The stored reducer has no current guided authoring recipe.");}else{result.key=recipe->recipe_id;for(std::size_t i=0;i<recipe->arguments.size();++i)if(!recipe->arguments[i].automatic_semantic_input_role){const auto child=Node(node.operands[i],path+"/"+recipe->arguments[i].key);if(child)result.children.push_back(*child);}}}
        else{result.kind=GuidedKind(node.kind);for(std::size_t i=0;i<node.operands.size();++i){const auto child=Node(node.operands[i],path+"/"+std::to_string(i));if(child)result.children.push_back(*child);}if(result.kind==PredicateGuidedNodeKindV1::All||result.kind==PredicateGuidedNodeKindV1::Any){std::vector<PredicateGuidedNodeV1>flattened;for(auto&child:result.children){if(child.kind==result.kind)for(auto&grandchild:child.children)flattened.push_back(std::move(grandchild));else flattened.push_back(std::move(child));}result.children=std::move(flattened);}}
        active_.erase(index);return result;
    }
    const PredicateDefinition&definition_;const PredicateAuthoringCatalogV2&catalog_;std::set<std::size_t>active_;std::vector<PredicateAuthoringDiagnosticV1>diagnostics_;
};

} // namespace

PredicateGuidedCompileResultV1 CompileGuidedPredicateDefinitionV1(const PredicateGuidedNodeV1&root,const PredicateAuthoringCatalogV2&catalog){return GuidedCompiler(catalog).Compile(root);}
PredicateGuidedReconstructionResultV1 ReconstructGuidedPredicateDefinitionV1(const PredicateDefinition&definition,const PredicateAuthoringCatalogV2&catalog){return GuidedReconstructor(definition,catalog).Run();}

std::optional<PredicateWitnessSourceBindingV1>
PlanPredicateSemanticWitnessSourceV1(
    const PredicateWitness& witness,
    std::uint32_t witness_ordinal,
    const PredicateAuthoringCatalogV2& catalog)
{
    const auto semantic = std::ranges::find_if(
        catalog.semantic_inputs,
        [&](const PredicateAuthoringSemanticInputV2& input) {
            return input.witness_name == witness.name &&
                   input.value_type == witness.value_type;
        });
    if (semantic == catalog.semantic_inputs.end() ||
        !semantic->recommended_query)
        return std::nullopt;
    const auto query = std::ranges::find(
        catalog.query_sources, *semantic->recommended_query,
        &PredicateAuthoringSourceV1::identity);
    if (query == catalog.query_sources.end() ||
        query->result_type != witness.value_type ||
        query->capture_hook_ids.empty() ||
        query->evaluation_hook_ids.empty())
        return std::nullopt;
    return PredicateWitnessSourceBindingV1{
        .witness_ordinal = witness_ordinal,
        .source_kind = PredicateWitnessSourceKindV1::DerivedStateQuery,
        .value_type = witness.value_type,
        .source = query->identity,
        .observation_source_kind =
            PredicateObservationSourceKindV1::RegisteredQuery,
    };
}

ResolvedPredicateGroupV1 EmptyPredicateGroupV1()
{
    ResolvedPredicateGroupV1 value{.predicate_group_revision_id=1,.canonical_id=std::string(kEmptyGroupId),.revision=1};value.content_sha256=ComputeResolvedPredicateGroupHashV1(value);return value;
}

PredicateExecutionPackageV1 EmptyPredicateExecutionPackageV1()
{
    PredicateExecutionPackageV1 value{.hook_contract=BattlePredicateHookContractV1(),.group=EmptyPredicateGroupV1()};value.content_sha256=ComputePredicateExecutionPackageHashV1(value);return value;
}

std::string ComputePredicateHookContractHashV1(const PredicateHookContractV1&v){Writer w;EncodeHook(w,v,false);return Hash(w.bytes);}
std::string ComputePredicateDefinitionHashV1(const PredicateDefinition&v)
{
    ProgramModule module{
        .identity = {
            .canonical_id = "authoring.predicate." + v.canonical_id,
            .revision = v.revision,
        },
    };
    PredicateEvaluationPolicy policy{
        .predicate_group_revision_id = 1,
        .execution_binding_revision_id = 1,
        .semantic_point_id = "authoring.validation",
    };
    if (!LowerPredicate(v, policy, module)) return {};
    module.identity.module_hash = ComputeProgramModuleHashV1(module);
    return module.identity.module_hash.ToHex();
}
std::string ComputePredicateDefinitionSemanticHashV1(const PredicateDefinition&v)
{
    auto semantic = v;
    semantic.canonical_id = "semantic";
    semantic.revision = 1;
    semantic.source_name.clear();
    return ComputePredicateDefinitionHashV1(semantic);
}
std::string ComputePredicateExecutionBindingHashV1(const PredicateExecutionBindingV1&v){Writer w;EncodeBinding(w,v,false);return Hash(w.bytes);}
std::string ComputePredicateExecutionBindingSemanticHashV1(const PredicateExecutionBindingV1&v)
{
    auto semantic = v;
    semantic.execution_binding_revision_id = 1;
    semantic.canonical_id = "semantic";
    semantic.revision = 1;
    semantic.content_sha256.clear();
    // The exact Definition identity is semantic dependency content, but its
    // database-local row id is not.
    semantic.definition.revision_id = 1;
    return ComputePredicateExecutionBindingHashV1(semantic);
}
std::string ComputeResolvedPredicateGroupHashV1(const ResolvedPredicateGroupV1&v){if(v.canonical_id==kEmptyGroupId&&v.members.empty())return hash::sha256(nullptr,0);Writer w;EncodeGroup(w,v,false);return Hash(w.bytes);}
std::string ComputeResolvedPredicateGroupSemanticHashV1(const ResolvedPredicateGroupV1&v)
{
    auto semantic = v;
    semantic.predicate_group_revision_id = 1;
    semantic.canonical_id = "semantic";
    semantic.revision = 1;
    semantic.content_sha256.clear();
    return ComputeResolvedPredicateGroupHashV1(semantic);
}
std::string ComputePredicateExecutionPackageHashV1(const PredicateExecutionPackageV1&v){Writer w;EncodeHook(w,v.hook_contract,true);EncodeGroup(w,v.group,true);w.U32(static_cast<std::uint32_t>(v.execution_bindings.size()));for(const auto&b:v.execution_bindings)EncodeBinding(w,b,true);return Hash(w.bytes);}
std::string ComputePredicateAuthoringCatalogHashV2(const PredicateAuthoringCatalogV2&v)
{
    Writer w;
    EncodeHook(w,v.hook_contract,true);
    w.U32(static_cast<std::uint32_t>(v.hooks.size()));
    for(const auto&h:v.hooks){w.String(h.canonical_id);w.String(h.display_name);w.String(h.description);w.U32(h.pc);}
    for(const auto&list:{&v.query_sources,&v.reducers}){w.U32(static_cast<std::uint32_t>(list->size()));for(const auto&s:*list){w.String(s.display_name);w.String(s.category);w.String(s.description);w.U8(static_cast<std::uint8_t>(s.source_kind));EncodeDependency(w,s.identity);w.U32(static_cast<std::uint32_t>(s.argument_types.size()));for(const auto&t:s.argument_types)EncodeType(w,t);EncodeType(w,s.result_type);w.U32(static_cast<std::uint32_t>(s.capture_hook_ids.size()));for(const auto&h:s.capture_hook_ids)w.String(h);w.U32(static_cast<std::uint32_t>(s.evaluation_hook_ids.size()));for(const auto&h:s.evaluation_hook_ids)w.String(h);}}
    w.U32(static_cast<std::uint32_t>(v.hook_receipt_fields.size()));for(const auto&f:v.hook_receipt_fields){w.String(f.field_name);w.String(f.display_name);w.String(f.description);EncodeType(w,f.value_type);}
    w.U32(static_cast<std::uint32_t>(v.semantic_inputs.size()));for(const auto&i:v.semantic_inputs){w.String(i.role_id);w.String(i.witness_name);w.String(i.display_name);w.String(i.description);EncodeType(w,i.value_type);w.U8(i.recommended_query?1:0);if(i.recommended_query)EncodeDependency(w,*i.recommended_query);}
    w.U32(static_cast<std::uint32_t>(v.value_recipes.size()));for(const auto&r:v.value_recipes){w.String(r.recipe_id);w.String(r.category);w.String(r.display_name);w.String(r.description);EncodeDependency(w,r.reducer);w.U32(static_cast<std::uint32_t>(r.arguments.size()));for(const auto&a:r.arguments){w.String(a.key);w.String(a.display_name);w.String(a.description);EncodeType(w,a.value_type);w.U8(a.automatic_semantic_input_role?1:0);if(a.automatic_semantic_input_role)w.String(*a.automatic_semantic_input_role);w.U8(a.suggested_parameter_name?1:0);if(a.suggested_parameter_name)w.String(*a.suggested_parameter_name);}EncodeType(w,r.result_type);}
    for(const auto*operators:{&v.comparison_operators,&v.calculation_operators}){w.U32(static_cast<std::uint32_t>(operators->size()));for(const auto&o:*operators){w.String(o.operator_id);w.String(o.display_name);w.String(o.description);w.U8(static_cast<std::uint8_t>(o.node_kind));w.U8(o.ordered_only?1:0);}}
    w.U32(static_cast<std::uint32_t>(v.value_types.size()));for(const auto&t:v.value_types)EncodeType(w,t);return Hash(w.bytes);
}

PredicateExecutionPackageValidationResult ValidatePredicateExecutionPackageV1(const PredicateExecutionPackageV1&p)
{
    if(p.hook_contract.canonical_id.empty()||p.hook_contract.revision==0||p.hook_contract.content_sha256!=ComputePredicateHookContractHashV1(p.hook_contract))return Invalid("predicate.hook_contract_invalid","predicate hook contract identity or hash is invalid");
    std::set<std::string>hooks;for(const auto&h:p.hook_contract.points)if(h.canonical_id.empty()||h.pc==0||!hooks.insert(h.canonical_id).second)return Invalid("predicate.hook_contract_invalid","predicate hook contract has an invalid or duplicate hook");
    if(p.group.predicate_group_revision_id<=0||p.group.canonical_id.empty()||p.group.revision==0||p.group.content_sha256!=ComputeResolvedPredicateGroupHashV1(p.group))return Invalid("predicate.group_hash_mismatch","predicate group identity or hash is invalid");
    if(p.content_sha256!=ComputePredicateExecutionPackageHashV1(PredicateExecutionPackageV1{p.hook_contract,p.group,p.execution_bindings,{}}))return Invalid("predicate.package_hash_mismatch","predicate execution package hash is invalid");
    if(p.group.canonical_id==kEmptyGroupId){if(!p.group.members.empty()||!p.execution_bindings.empty())return Invalid("predicate.empty_group_invalid","canonical empty predicate group carries authored content");return{true,{},{}};}
    const auto catalog=BattlePredicateAuthoringCatalogV2();std::set<std::int64_t>binding_ids;
    for(const auto&b:p.execution_bindings){if(b.execution_binding_revision_id<=0||b.canonical_id.empty()||b.revision==0||!binding_ids.insert(b.execution_binding_revision_id).second||b.content_sha256!=ComputePredicateExecutionBindingHashV1(b))return Invalid("predicate.binding_invalid","predicate execution binding identity or hash is invalid");if(b.definition.revision_id<=0||b.definition.content_sha256!=ComputePredicateDefinitionHashV1(b.definition.definition)||b.witnesses.size()!=b.definition.definition.witnesses.size())return Invalid("predicate.definition_invalid","execution binding definition or witness count is invalid");for(std::size_t i=0;i<b.witnesses.size();++i){const auto&s=b.witnesses[i];if(s.witness_ordinal!=i||s.value_type!=b.definition.definition.witnesses[i].value_type)return Invalid("predicate.witness_binding_invalid","execution binding witness order or type is invalid");if(s.source_kind==PredicateWitnessSourceKindV1::ConcreteValue){if(!s.concrete_value||s.concrete_value->type!=s.value_type||s.source||s.pinned_guest_address||!s.baseline_capture_hook_id.empty())return Invalid("predicate.witness_binding_invalid","concrete witness source is malformed");}else if(s.concrete_value)return Invalid("predicate.witness_binding_invalid","non-concrete witness carries a concrete value");if(s.source_kind==PredicateWitnessSourceKindV1::CurrentHookReceipt&&!ReceiptFieldValid(s.source_field,s.value_type))return Invalid("predicate.witness_binding_invalid","hook receipt field or type is unavailable");if(s.source_kind==PredicateWitnessSourceKindV1::PinnedGuestMemory){const auto a=GuestReadFor(s.value_type);if(!a||!s.source||*s.source!=CanonicalActionIdentity(*a)||!s.pinned_guest_address)return Invalid("predicate.witness_binding_invalid","pinned guest-memory source is malformed");}if(s.source_kind==PredicateWitnessSourceKindV1::DerivedStateQuery&&(!s.source||s.observation_source_kind!=PredicateObservationSourceKindV1::RegisteredQuery))return Invalid("predicate.witness_binding_invalid","derived-state witness requires one exact registered query");if(s.source_kind==PredicateWitnessSourceKindV1::BaselineObservation&&(!hooks.contains(s.baseline_capture_hook_id)||s.observation_source_kind==PredicateObservationSourceKindV1::HookReceipt&&!ReceiptFieldValid(s.source_field,s.value_type)))return Invalid("predicate.baseline_invalid","baseline witness source is incomplete");}}
    std::set<std::int64_t>members;std::set<std::int64_t>required_bindings;for(std::size_t i=0;i<p.group.members.size();++i){const auto&m=p.group.members[i];if(m.ordinal!=i||!members.insert(m.execution_binding_revision_id).second||m.semantic_hook_ids.empty()||!std::ranges::is_sorted(m.semantic_hook_ids)||std::adjacent_find(m.semantic_hook_ids.begin(),m.semantic_hook_ids.end())!=m.semantic_hook_ids.end())return Invalid("predicate.group_member_invalid","group members require ordered unique bindings and a sorted nonempty hook set");required_bindings.insert(m.execution_binding_revision_id);const auto*b=FindBinding(p,m.execution_binding_revision_id);if(!b)return Invalid("predicate.group_member_invalid","group member references an unavailable execution binding");for(const auto&h:m.semantic_hook_ids){if(!hooks.contains(h))return Invalid("predicate.hook_unavailable","group member hook is unavailable");for(const auto&s:b->witnesses)if(s.source_kind==PredicateWitnessSourceKindV1::DerivedStateQuery&&!SourceSchedulableAt(catalog,s,h))return Invalid("predicate.observation_unschedulable","required Battle data cannot be captured before every selected evaluation hook");}if((m.occurrence==PredicateOccurrencePolicyV1::Ordinal)!=(m.occurrence_ordinal.has_value())||(m.occurrence==PredicateOccurrencePolicyV1::GuardOnce)!=(m.guard_execution_binding_revision_id.has_value()))return Invalid("predicate.occurrence_invalid","group member occurrence configuration is incomplete");if(m.guard_execution_binding_revision_id){required_bindings.insert(*m.guard_execution_binding_revision_id);const auto*g=FindBinding(p,*m.guard_execution_binding_revision_id);if(!g||g->definition.definition.witnesses!=b->definition.definition.witnesses||g->witnesses!=b->witnesses)return Invalid("predicate.occurrence_invalid","GuardOnce requires a compatible published execution binding");}for(const auto&h:m.semantic_hook_ids){ProgramModule module;PredicateEvaluationPolicy policy{.predicate_group_revision_id=p.group.predicate_group_revision_id,.execution_binding_revision_id=m.execution_binding_revision_id,.member_ordinal=m.ordinal,.semantic_point_id=h,.reaction=m.reaction,.emit_evidence=m.emit_evidence,.participates_in_aggregation=m.participates_in_aggregation};const auto lowered=LowerPredicate(b->definition.definition,policy,module);if(!lowered)return Invalid(lowered.diagnostics.empty()?"predicate.lowering_failed":lowered.diagnostics.front().code,lowered.diagnostics.empty()?"predicate could not be lowered":lowered.diagnostics.front().message);}}
    if(binding_ids!=required_bindings)return Invalid("predicate.package_binding_mismatch","execution package must contain exactly the bindings referenced by the group");return{true,{},{}};
}

bool EncodePredicateExecutionPackageV1(const PredicateExecutionPackageV1&p,std::vector<std::uint8_t>&out,std::string*d)
{
    const auto validation=ValidatePredicateExecutionPackageV1(p);if(!validation){SetDiagnostic(d,validation.code+": "+validation.message);return false;}Writer w;w.U32(PredicateExecutionPackageWireVersionV1);EncodeHook(w,p.hook_contract,true);EncodeGroup(w,p.group,true);w.U32(static_cast<std::uint32_t>(p.execution_bindings.size()));for(const auto&b:p.execution_bindings)EncodeBinding(w,b,true);w.String(p.content_sha256);out=std::move(w.bytes);SetDiagnostic(d,{});return true;
}

bool DecodePredicateExecutionPackageV1(std::span<const std::uint8_t>input,PredicateExecutionPackageV1&out,std::string*d)
{
    Reader r(input);PredicateExecutionPackageV1 value;std::uint32_t version=0,n=0;if(!r.U32(version)||version!=PredicateExecutionPackageWireVersionV1||!DecodeHook(r,value.hook_contract)||!DecodeGroup(r,value.group)||!r.Count(n)){SetDiagnostic(d,"predicate execution package is malformed");return false;}value.execution_bindings.resize(n);for(auto&b:value.execution_bindings)if(!DecodeBinding(r,b)){SetDiagnostic(d,"predicate execution package is malformed");return false;}if(!r.String(value.content_sha256)||!r.Done()){SetDiagnostic(d,"predicate execution package is malformed");return false;}const auto validation=ValidatePredicateExecutionPackageV1(value);if(!validation){SetDiagnostic(d,validation.code+": "+validation.message);return false;}out=std::move(value);SetDiagnostic(d,{});return true;
}

} // namespace savor::runtime::predicates
