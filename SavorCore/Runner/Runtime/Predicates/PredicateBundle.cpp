#include "PredicateBundle.h"

#include "../../Breakpoints/BpRegistry.h"
#include "../ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "../../../Utils/Hash.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <map>
#include <set>
#include <type_traits>

namespace savor::runtime::predicates {
namespace {

using namespace program;
using namespace program::composition;

constexpr std::size_t kMaximumString = 64 * 1024;
constexpr std::size_t kMaximumBlob = 1024 * 1024;
constexpr std::size_t kMaximumEntries = 4096;
constexpr std::string_view kEmptyBundleId = "savor.predicate_bundle.empty";

void SetDiagnostic(std::string* output, std::string value)
{
    if (output) *output = std::move(value);
}

class Writer
{
public:
    void U8(std::uint8_t value) { bytes.push_back(value); }
    void U32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void U64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void I64(std::int64_t value) { U64(std::bit_cast<std::uint64_t>(value)); }
    void String(std::string_view value) {
        U32(static_cast<std::uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void Blob(std::span<const std::uint8_t> value) {
        U32(static_cast<std::uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    std::vector<std::uint8_t> bytes;
};

class Reader
{
public:
    explicit Reader(std::span<const std::uint8_t> source) : source_(source) {}
    bool U8(std::uint8_t& value) {
        if (offset_ >= source_.size()) return false;
        value = source_[offset_++]; return true;
    }
    bool U32(std::uint32_t& value) {
        if (source_.size() - offset_ < 4) return false;
        value = 0;
        for (int shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(source_[offset_++]) << shift;
        return true;
    }
    bool U64(std::uint64_t& value) {
        if (source_.size() - offset_ < 8) return false;
        value = 0;
        for (int shift = 0; shift < 64; shift += 8)
            value |= static_cast<std::uint64_t>(source_[offset_++]) << shift;
        return true;
    }
    bool I64(std::int64_t& value) {
        std::uint64_t raw = 0; if (!U64(raw)) return false;
        value = std::bit_cast<std::int64_t>(raw); return true;
    }
    bool String(std::string& value) {
        std::uint32_t size = 0;
        if (!U32(size) || size > kMaximumString || source_.size() - offset_ < size) return false;
        value.assign(reinterpret_cast<const char*>(source_.data() + offset_), size);
        offset_ += size; return true;
    }
    bool Blob(std::vector<std::uint8_t>& value) {
        std::uint32_t size = 0;
        if (!U32(size) || size > kMaximumBlob || source_.size() - offset_ < size) return false;
        value.assign(source_.begin() + offset_, source_.begin() + offset_ + size);
        offset_ += size; return true;
    }
    bool Done() const { return offset_ == source_.size(); }
private:
    std::span<const std::uint8_t> source_;
    std::size_t offset_ = 0;
};

void EncodeType(Writer& writer, const TypeRef& type)
{
    writer.U8(type.is_named() ? 1 : 0);
    if (!type.is_named()) { writer.U8(static_cast<std::uint8_t>(type.builtin)); return; }
    writer.String(type.named->canonical_id);
    writer.U32(type.named->version);
    writer.String(type.named->schema_hash.ToHex());
}

bool DecodeType(Reader& reader, TypeRef& type)
{
    std::uint8_t named = 0;
    if (!reader.U8(named) || named > 1) return false;
    if (!named) {
        std::uint8_t builtin = 0;
        if (!reader.U8(builtin) || builtin > static_cast<std::uint8_t>(BuiltinType::F64)) return false;
        type = TypeRef::Builtin(static_cast<BuiltinType>(builtin)); return true;
    }
    std::string id, hash_text;
    std::uint32_t revision = 0;
    if (!reader.String(id) || !reader.U32(revision) || !reader.String(hash_text)) return false;
    const auto hash_value = ContentHash256::FromHex(hash_text);
    if (id.empty() || revision == 0 || !hash_value) return false;
    type = TypeRef::Named({id, revision, *hash_value}); return true;
}

void EncodeDependency(Writer& writer, const ExactDependencyIdentity& value)
{
    writer.String(value.canonical_id);
    writer.U32(value.version);
    writer.String(value.signature_hash.ToHex());
}

bool DecodeDependency(Reader& reader, ExactDependencyIdentity& value)
{
    std::string hash_text;
    if (!reader.String(value.canonical_id) || !reader.U32(value.version)
        || !reader.String(hash_text)) return false;
    const auto hash_value = ContentHash256::FromHex(hash_text);
    if (value.canonical_id.empty() || value.version == 0 || !hash_value) return false;
    value.signature_hash = *hash_value; return true;
}

void EncodeLiteral(Writer& writer, const LiteralValue& value)
{
    EncodeType(writer, value.type);
    writer.U8(static_cast<std::uint8_t>(value.payload.index()));
    std::visit([&](const auto& payload) {
        using Value = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Value, UnitValue>) {}
        else if constexpr (std::is_same_v<Value, bool>) writer.U8(payload ? 1 : 0);
        else if constexpr (std::is_same_v<Value, std::uint8_t>) writer.U8(payload);
        else if constexpr (std::is_same_v<Value, std::uint16_t>) writer.U32(payload);
        else if constexpr (std::is_same_v<Value, std::uint32_t>) writer.U32(payload);
        else if constexpr (std::is_same_v<Value, std::uint64_t>) writer.U64(payload);
        else if constexpr (std::is_same_v<Value, std::int32_t>) writer.I64(payload);
        else if constexpr (std::is_same_v<Value, std::int64_t>) writer.I64(payload);
        else if constexpr (std::is_same_v<Value, float>) writer.U32(std::bit_cast<std::uint32_t>(payload));
        else if constexpr (std::is_same_v<Value, double>) writer.U64(std::bit_cast<std::uint64_t>(payload));
        else if constexpr (std::is_same_v<Value, std::string>) writer.String(payload);
        else if constexpr (std::is_same_v<Value, std::vector<Byte>>) writer.Blob(payload);
        else if constexpr (std::is_same_v<Value, EnumValue>) {
            writer.String(payload.schema.canonical_id); writer.U32(payload.schema.version);
            writer.String(payload.schema.schema_hash.ToHex()); writer.I64(payload.value);
        }
    }, value.payload);
}

bool DecodeLiteral(Reader& reader, LiteralValue& value)
{
    if (!DecodeType(reader, value.type)) return false;
    std::uint8_t kind = 0;
    if (!reader.U8(kind) || kind >= std::variant_size_v<LiteralPayload>) return false;
    switch (kind)
    {
    case 0: value.payload = UnitValue{}; return true;
    case 1: { std::uint8_t v=0; if (!reader.U8(v) || v>1) return false; value.payload = v != 0; return true; }
    case 2: { std::uint8_t v=0; if (!reader.U8(v)) return false; value.payload=v; return true; }
    case 3: { std::uint32_t v=0; if (!reader.U32(v) || v>65535) return false; value.payload=static_cast<std::uint16_t>(v); return true; }
    case 4: { std::uint32_t v=0; if (!reader.U32(v)) return false; value.payload=v; return true; }
    case 5: { std::uint64_t v=0; if (!reader.U64(v)) return false; value.payload=v; return true; }
    case 6: { std::int64_t v=0; if (!reader.I64(v) || v < (std::numeric_limits<std::int32_t>::min)() || v > (std::numeric_limits<std::int32_t>::max)()) return false; value.payload=static_cast<std::int32_t>(v); return true; }
    case 7: { std::int64_t v=0; if (!reader.I64(v)) return false; value.payload=v; return true; }
    case 8: { std::uint32_t v=0; if (!reader.U32(v)) return false; value.payload=std::bit_cast<float>(v); return true; }
    case 9: { std::uint64_t v=0; if (!reader.U64(v)) return false; value.payload=std::bit_cast<double>(v); return true; }
    case 10: { std::string v; if (!reader.String(v)) return false; value.payload=std::move(v); return true; }
    case 11: { std::vector<std::uint8_t> v; if (!reader.Blob(v)) return false; value.payload=std::move(v); return true; }
    case 12: {
        EnumValue v; std::string hash_text; std::int64_t integer = 0;
        if (!reader.String(v.schema.canonical_id) || !reader.U32(v.schema.version)
            || !reader.String(hash_text) || !reader.I64(integer)) return false;
        const auto hash_value = ContentHash256::FromHex(hash_text);
        if (!hash_value) return false;
        v.schema.schema_hash = *hash_value; v.value = integer; value.payload = std::move(v); return true;
    }
    default: return false;
    }
}

void EncodeDefinition(Writer& w, const ResolvedPredicateDefinitionV1& value)
{
    w.I64(value.revision_id); w.String(value.definition.canonical_id);
    w.U32(value.definition.revision); w.String(value.definition.source_name);
    w.U32(static_cast<std::uint32_t>(value.definition.witnesses.size()));
    for (const auto& witness : value.definition.witnesses) {
        w.String(witness.name); EncodeType(w, witness.value_type);
    }
    w.U32(static_cast<std::uint32_t>(value.definition.expression.size()));
    for (const auto& node : value.definition.expression) {
        w.U8(static_cast<std::uint8_t>(node.kind));
        w.U8(node.witness_index ? 1 : 0); if (node.witness_index) w.U32(static_cast<std::uint32_t>(*node.witness_index));
        w.U8(node.literal ? 1 : 0); if (node.literal) EncodeLiteral(w, *node.literal);
        w.U32(static_cast<std::uint32_t>(node.operands.size())); for (auto operand : node.operands) w.U32(static_cast<std::uint32_t>(operand));
        w.U8(node.reducer ? 1 : 0); if (node.reducer) EncodeDependency(w, *node.reducer);
        EncodeType(w, node.result_type); w.String(node.source_label);
    }
    w.U32(static_cast<std::uint32_t>(value.definition.root_expression));
}

bool Count(Reader& reader, std::uint32_t& count)
{
    return reader.U32(count) && count <= kMaximumEntries;
}

bool DecodeDefinition(Reader& r, ResolvedPredicateDefinitionV1& value)
{
    std::int64_t revision_id = 0; std::uint32_t count = 0;
    if (!r.I64(revision_id) || revision_id <= 0 || !r.String(value.definition.canonical_id)
        || !r.U32(value.definition.revision) || !r.String(value.definition.source_name)
        || !Count(r, count)) return false;
    value.revision_id = revision_id; value.definition.witnesses.resize(count);
    for (auto& witness : value.definition.witnesses)
        if (!r.String(witness.name) || !DecodeType(r, witness.value_type)) return false;
    if (!Count(r, count)) return false; value.definition.expression.resize(count);
    for (auto& node : value.definition.expression) {
        std::uint8_t kind=0, present=0; std::uint32_t integer=0;
        if (!r.U8(kind) || kind > static_cast<std::uint8_t>(PredicateExpressionKind::ImportedReducer)
            || !r.U8(present) || present>1) return false;
        node.kind=static_cast<PredicateExpressionKind>(kind);
        if (present) { if (!r.U32(integer)) return false; node.witness_index=integer; }
        if (!r.U8(present) || present>1) return false;
        if (present) { LiteralValue literal; if (!DecodeLiteral(r,literal)) return false; node.literal=std::move(literal); }
        if (!Count(r,count)) return false; node.operands.resize(count);
        for (auto& operand : node.operands) { if (!r.U32(integer)) return false; operand=integer; }
        if (!r.U8(present) || present>1) return false;
        if (present) { ExactDependencyIdentity dependency; if (!DecodeDependency(r,dependency)) return false; node.reducer=std::move(dependency); }
        if (!DecodeType(r,node.result_type) || !r.String(node.source_label)) return false;
    }
    if (!r.U32(count)) return false; value.definition.root_expression=count; return true;
}

void EncodeCheckUse(Writer& w, const PredicateCheckUse& value)
{
    w.String(value.canonical_id); w.String(value.semantic_point_id);
    w.U8(static_cast<std::uint8_t>(value.reaction)); w.U8(value.emit_evidence?1:0);
    w.U8(value.participates_in_aggregation?1:0);
}

bool DecodeCheckUse(Reader& r, PredicateCheckUse& value)
{
    std::uint8_t raw=0,present=0;
    if (!r.String(value.canonical_id) || !r.String(value.semantic_point_id)
        || !r.U8(raw) || raw>1 || !r.U8(present) || present>1) return false;
    value.reaction=static_cast<PredicateReaction>(raw); value.emit_evidence=present!=0;
    if (!r.U8(present) || present>1) return false; value.participates_in_aggregation=present!=0;
    return true;
}

void EncodeHook(Writer& w, const PredicateHookContractV1& value, bool include_hash)
{
    w.String(value.canonical_id); w.U32(value.revision);
    w.String(include_hash ? value.content_sha256 : std::string_view{});
    w.U32(static_cast<std::uint32_t>(value.points.size()));
    for (const auto& point : value.points) { w.String(point.canonical_id); w.U32(point.pc); }
}

bool DecodeHook(Reader& r, PredicateHookContractV1& value)
{
    std::uint32_t count=0;
    if (!r.String(value.canonical_id)||!r.U32(value.revision)||!r.String(value.content_sha256)||!Count(r,count)) return false;
    value.points.resize(count);
    for(auto& point:value.points) if(!r.String(point.canonical_id)||!r.U32(point.pc)) return false;
    return true;
}

void EncodeBundle(Writer& w, const ResolvedPredicateBundleV1& value, bool include_hash)
{
    w.I64(value.bundle_revision_id); w.String(value.canonical_id); w.U32(value.revision);
    w.String(include_hash ? value.content_sha256 : std::string_view{});
    w.U32(static_cast<std::uint32_t>(value.definitions.size())); for(const auto& item:value.definitions) EncodeDefinition(w,item);
    w.U32(static_cast<std::uint32_t>(value.parameters.size()));
    for(const auto& item:value.parameters){w.U32(item.ordinal);w.String(item.name);EncodeType(w,item.value_type);}
    w.U32(static_cast<std::uint32_t>(value.observations.size()));
    for(const auto& item:value.observations){w.U32(item.ordinal);w.String(item.stable_key);w.String(item.semantic_hook_id);w.U8(static_cast<std::uint8_t>(item.source_kind));EncodeDependency(w,item.source);w.U8(item.pinned_guest_address?1:0);if(item.pinned_guest_address)w.U64(*item.pinned_guest_address);w.String(item.source_field);EncodeType(w,item.value_type);}
    w.U32(static_cast<std::uint32_t>(value.baselines.size()));
    for(const auto& item:value.baselines){w.U32(item.ordinal);w.String(item.name);w.String(item.capture_hook_id);w.U32(item.observation_ordinal);w.U8(static_cast<std::uint8_t>(item.update_policy));}
    w.U32(static_cast<std::uint32_t>(value.checks.size()));
    for(const auto& item:value.checks){
        w.U32(item.ordinal);w.I64(item.predicate_definition_revision_id);EncodeCheckUse(w,item.use);w.U8(static_cast<std::uint8_t>(item.occurrence));
        w.U8(item.occurrence_ordinal?1:0);if(item.occurrence_ordinal)w.U32(*item.occurrence_ordinal);
        w.U8(item.guard_predicate_definition_revision_id?1:0);if(item.guard_predicate_definition_revision_id)w.I64(*item.guard_predicate_definition_revision_id);
        w.U32(static_cast<std::uint32_t>(item.witnesses.size()));
        for(const auto& witness:item.witnesses){w.U32(witness.witness_ordinal);w.U8(static_cast<std::uint8_t>(witness.source_kind));w.U8(witness.source_ordinal?1:0);if(witness.source_ordinal)w.U32(*witness.source_ordinal);w.String(witness.source_field);EncodeType(w,witness.value_type);w.U8(witness.literal?1:0);if(witness.literal)EncodeLiteral(w,*witness.literal);}
    }
}

bool DecodeBundle(Reader& r, ResolvedPredicateBundleV1& value)
{
    std::int64_t id=0;std::uint32_t count=0;std::uint8_t raw=0,present=0;
    if(!r.I64(id)||id<=0||!r.String(value.canonical_id)||!r.U32(value.revision)||!r.String(value.content_sha256)||!Count(r,count))return false;value.bundle_revision_id=id;
    value.definitions.resize(count);for(auto& item:value.definitions)if(!DecodeDefinition(r,item))return false;
    if(!Count(r,count))return false;value.parameters.resize(count);for(auto& item:value.parameters)if(!r.U32(item.ordinal)||!r.String(item.name)||!DecodeType(r,item.value_type))return false;
    if(!Count(r,count))return false;value.observations.resize(count);for(auto& item:value.observations){std::uint8_t present=0;std::uint64_t address=0;if(!r.U32(item.ordinal)||!r.String(item.stable_key)||!r.String(item.semantic_hook_id)||!r.U8(raw)||raw>3||!DecodeDependency(r,item.source)||!r.U8(present)||present>1||(present&&!r.U64(address))||!r.String(item.source_field)||!DecodeType(r,item.value_type))return false;if(present)item.pinned_guest_address=address;item.source_kind=static_cast<PredicateObservationSourceKindV1>(raw);}
    if(!Count(r,count))return false;value.baselines.resize(count);for(auto& item:value.baselines){if(!r.U32(item.ordinal)||!r.String(item.name)||!r.String(item.capture_hook_id)||!r.U32(item.observation_ordinal)||!r.U8(raw)||raw>1)return false;item.update_policy=static_cast<PredicateBaselineUpdatePolicyV1>(raw);}
    if(!Count(r,count))return false;value.checks.resize(count);for(auto& item:value.checks){
        if(!r.U32(item.ordinal)||!r.I64(item.predicate_definition_revision_id)||!DecodeCheckUse(r,item.use)||!r.U8(raw)||raw>3||!r.U8(present)||present>1)return false;item.occurrence=static_cast<PredicateOccurrencePolicyV1>(raw);
        if(present){std::uint32_t ordinal=0;if(!r.U32(ordinal))return false;item.occurrence_ordinal=ordinal;}
        if(!r.U8(present)||present>1)return false;if(present){std::int64_t guard=0;if(!r.I64(guard))return false;item.guard_predicate_definition_revision_id=guard;}
        if(!Count(r,count))return false;item.witnesses.resize(count);for(auto& witness:item.witnesses){if(!r.U32(witness.witness_ordinal)||!r.U8(raw)||raw>4||!r.U8(present)||present>1)return false;witness.source_kind=static_cast<PredicateWitnessSourceKindV1>(raw);if(present){std::uint32_t ordinal=0;if(!r.U32(ordinal))return false;witness.source_ordinal=ordinal;}if(!r.String(witness.source_field)||!DecodeType(r,witness.value_type)||!r.U8(present)||present>1)return false;if(present){LiteralValue literal;if(!DecodeLiteral(r,literal))return false;witness.literal=std::move(literal);}}
    }return true;
}

void EncodeBinding(Writer& w, const PredicateBundleBindingV1& value, bool include_hash)
{
    w.I64(value.bundle_revision_id);w.String(value.bundle_content_sha256);
    w.U32(static_cast<std::uint32_t>(value.parameter_values.size()));for(const auto& item:value.parameter_values)EncodeLiteral(w,item);
    w.U32(static_cast<std::uint32_t>(value.active_check_ordinals.size()));for(auto item:value.active_check_ordinals)w.U32(item);
    w.U8(static_cast<std::uint8_t>(value.aggregation));w.String(value.structural_active_check_sha256);
    w.String(include_hash?value.content_sha256:std::string_view{});
}

bool DecodeBinding(Reader& r, PredicateBundleBindingV1& value)
{
    std::int64_t id=0;std::uint32_t count=0;std::uint8_t raw=0;
    if(!r.I64(id)||id<=0||!r.String(value.bundle_content_sha256)||!Count(r,count))return false;value.bundle_revision_id=id;
    value.parameter_values.resize(count);for(auto& item:value.parameter_values)if(!DecodeLiteral(r,item))return false;
    if(!Count(r,count))return false;value.active_check_ordinals.resize(count);for(auto& item:value.active_check_ordinals)if(!r.U32(item))return false;
    if(!r.U8(raw)||raw>0||!r.String(value.structural_active_check_sha256)||!r.String(value.content_sha256))return false;value.aggregation=static_cast<PredicateAggregationKindV1>(raw);return true;
}

std::string Hash(std::span<const std::uint8_t> bytes)
{
    return hash::sha256(bytes.data(), bytes.size());
}

std::string ActiveHash(std::span<const std::uint32_t> ordinals)
{
    Writer writer;for(auto ordinal:ordinals)writer.U32(ordinal);return Hash(writer.bytes);
}

PredicateBundleValidationResult Invalid(std::string code,std::string message)
{
    return {false,std::move(code),std::move(message)};
}

} // namespace

PredicateHookContractV1 BattlePredicateHookContractV1()
{
    PredicateHookContractV1 value{
        .canonical_id = "soa.battle.predicate_hooks",
        .revision = 1,
    };
    for (const auto& point : bp::BpRegistry::ForConsumer(BreakpointConsumer::Predicate)) {
        if (bp::domain_of(point.key) != bp::BPDomain::Battle
            || point.key == bp::battle::StartAction) continue;
        value.points.push_back({point.stable_id, point.pc});
    }
    std::ranges::sort(value.points, {}, &PredicateHookPointV1::canonical_id);
    value.content_sha256 = ComputePredicateHookContractHashV1(value);
    return value;
}

ResolvedPredicateBundleV1 EmptyPredicateBundleV1()
{
    ResolvedPredicateBundleV1 value{
        .bundle_revision_id = 1,
        .canonical_id = std::string(kEmptyBundleId),
        .revision = 1,
    };
    value.content_sha256 = ComputeResolvedPredicateBundleHashV1(value);
    return value;
}

PredicateBundleBindingV1 EmptyPredicateBundleBindingV1()
{
    const auto bundle = EmptyPredicateBundleV1();
    PredicateBundleBindingV1 value{
        .bundle_revision_id = bundle.bundle_revision_id,
        .bundle_content_sha256 = bundle.content_sha256,
        .structural_active_check_sha256 = ActiveHash(
            std::span<const std::uint32_t>{}),
    };
    value.content_sha256 = ComputePredicateBundleBindingHashV1(value);
    return value;
}

std::string ComputePredicateHookContractHashV1(const PredicateHookContractV1& value)
{
    Writer writer;EncodeHook(writer,value,false);return Hash(writer.bytes);
}

std::string ComputeResolvedPredicateBundleHashV1(const ResolvedPredicateBundleV1& value)
{
    if(value.canonical_id==kEmptyBundleId&&value.definitions.empty()&&value.parameters.empty()&&value.observations.empty()&&value.baselines.empty()&&value.checks.empty())
        return hash::sha256(nullptr,0);
    Writer writer;EncodeBundle(writer,value,false);return Hash(writer.bytes);
}

std::string ComputePredicateBundleBindingHashV1(const PredicateBundleBindingV1& value)
{
    Writer writer;EncodeBinding(writer,value,false);return Hash(writer.bytes);
}

std::string ComputePredicateActiveCheckSetHashV1(
    std::span<const std::uint32_t> active_check_ordinals)
{
    return ActiveHash(active_check_ordinals);
}

PredicateBundleValidationResult ValidatePredicateBundlePackageV1(
    const PredicateBundleExecutionPackageV1& package)
{
    if(package.hook_contract.canonical_id.empty()||package.hook_contract.revision==0
        ||package.hook_contract.content_sha256!=ComputePredicateHookContractHashV1(package.hook_contract))
        return Invalid("predicate.hook_contract_invalid","predicate hook contract identity or hash is invalid");
    std::set<std::string> hooks;
    for(const auto& point:package.hook_contract.points)
        if(point.canonical_id.empty()||point.pc==0||!hooks.insert(point.canonical_id).second)
            return Invalid("predicate.hook_contract_invalid","predicate hook contract contains duplicate or invalid points");
    for(const auto& point:package.hook_contract.points)
        if(point.canonical_id.find("StartAction")!=std::string::npos)
            return Invalid("predicate.hook_unavailable","StartAction is unavailable until a distinct address is validated");
    const auto& bundle=package.bundle;const auto& binding=package.binding;
    if(bundle.bundle_revision_id<=0||bundle.canonical_id.empty()||bundle.revision==0
        ||bundle.content_sha256!=ComputeResolvedPredicateBundleHashV1(bundle))
        return Invalid("predicate.bundle_hash_mismatch","resolved predicate bundle identity or hash is invalid");
    if(binding.bundle_revision_id!=bundle.bundle_revision_id||binding.bundle_content_sha256!=bundle.content_sha256
        ||binding.content_sha256!=ComputePredicateBundleBindingHashV1(binding))
        return Invalid("predicate.binding_hash_mismatch","predicate bundle binding does not match its exact revision");
    if(binding.parameter_values.size()!=bundle.parameters.size())
        return Invalid("predicate.parameter_mismatch","predicate binding must supply every typed parameter");
    for(std::size_t i=0;i<bundle.parameters.size();++i){
        if(bundle.parameters[i].ordinal!=i||bundle.parameters[i].name.empty()
            ||binding.parameter_values[i].type!=bundle.parameters[i].value_type)
            return Invalid("predicate.parameter_mismatch","predicate parameter ordinals or types are invalid");
    }
    if(!std::ranges::is_sorted(binding.active_check_ordinals)
        ||std::adjacent_find(binding.active_check_ordinals.begin(), binding.active_check_ordinals.end())!=binding.active_check_ordinals.end()
        ||binding.structural_active_check_sha256!=ActiveHash(binding.active_check_ordinals))
        return Invalid("predicate.active_check_mismatch","active predicate checks must be sorted, unique, and structurally hashed");
    std::map<std::int64_t,const PredicateDefinition*> definitions;
    for(const auto& item:bundle.definitions)
        if(item.revision_id<=0||!definitions.emplace(item.revision_id,&item.definition).second)
            return Invalid("predicate.definition_invalid","predicate definition revision identities must be unique");
    for(std::size_t i=0;i<bundle.observations.size();++i){
        const auto& item=bundle.observations[i];if(item.ordinal!=i||!hooks.contains(item.semantic_hook_id))
            return Invalid("predicate.hook_unavailable","predicate observation references an unavailable hook");
        if(item.stable_key.empty())
            return Invalid("predicate.observation_invalid","predicate observation requires a stable identity");
        if(item.source_kind==PredicateObservationSourceKindV1::GuestAddress){
            if(!item.pinned_guest_address||!item.source_field.empty()||item.value_type.is_named()
                ||(item.value_type.builtin!=BuiltinType::U8&&item.value_type.builtin!=BuiltinType::U16
                    &&item.value_type.builtin!=BuiltinType::U32&&item.value_type.builtin!=BuiltinType::U64))
                return Invalid("predicate.observation_invalid","guest-address observations require an explicit address and unsigned integer type");
            CanonicalAction action=CanonicalAction::GuestReadU8;
            if(item.value_type.builtin==BuiltinType::U16)action=CanonicalAction::GuestReadU16;
            else if(item.value_type.builtin==BuiltinType::U32)action=CanonicalAction::GuestReadU32;
            else if(item.value_type.builtin==BuiltinType::U64)action=CanonicalAction::GuestReadU64;
            if(item.source!=CanonicalActionIdentity(action))
                return Invalid("predicate.observation_invalid","guest-address observation source does not match its exact typed guest-read action");
        }else if(item.pinned_guest_address){
            return Invalid("predicate.observation_invalid","only guest-address observations may carry a pinned address");
        }
        if(item.source_kind==PredicateObservationSourceKindV1::HookReceipt){
            const bool u32=item.source_field=="pc"&&item.value_type==TypeRef::Builtin(BuiltinType::U32);
            const bool u64=(item.source_field=="movie_input_count"||item.source_field=="workset_epoch"||item.source_field=="vi_count")&&item.value_type==TypeRef::Builtin(BuiltinType::U64);
            if(!u32&&!u64)
                return Invalid("predicate.observation_invalid","hook-receipt observation field or exact type is unavailable");
        }
    }
    for(std::size_t i=0;i<bundle.baselines.size();++i){
        const auto& item=bundle.baselines[i];if(item.ordinal!=i||item.observation_ordinal>=bundle.observations.size()||!hooks.contains(item.capture_hook_id)
            ||bundle.observations[item.observation_ordinal].semantic_hook_id!=item.capture_hook_id)
            return Invalid("predicate.baseline_invalid","predicate baseline reference is invalid");
    }
    std::set<std::uint32_t> active(binding.active_check_ordinals.begin(),binding.active_check_ordinals.end());
    for(std::size_t i=0;i<bundle.checks.size();++i){
        const auto& check=bundle.checks[i];if(check.ordinal!=i||!hooks.contains(check.use.semantic_point_id))
            return Invalid("predicate.hook_unavailable","predicate check references an unavailable hook");
        if((check.occurrence==PredicateOccurrencePolicyV1::Ordinal)!=(check.occurrence_ordinal.has_value())
            ||(check.occurrence==PredicateOccurrencePolicyV1::GuardOnce)!=(check.guard_predicate_definition_revision_id.has_value()))
            return Invalid("predicate.occurrence_invalid","predicate occurrence policy binding is incomplete");
        const auto definition=definitions.find(check.predicate_definition_revision_id);
        if(definition==definitions.end()||check.witnesses.size()!=definition->second->witnesses.size())
            return Invalid("predicate.witness_binding_invalid","every required predicate witness must be bound exactly once");
        for(std::size_t witness_index=0;witness_index<check.witnesses.size();++witness_index){
            const auto& witness=check.witnesses[witness_index];
            if(witness.witness_ordinal!=witness_index||witness.value_type!=definition->second->witnesses[witness_index].value_type)
                return Invalid("predicate.witness_binding_invalid","predicate witness binding type is invalid");
            if(witness.source_kind==PredicateWitnessSourceKindV1::Literal){if(!witness.literal||witness.literal->type!=witness.value_type)return Invalid("predicate.witness_binding_invalid","literal witness binding is missing exact evidence");}
            else if(witness.literal)return Invalid("predicate.witness_binding_invalid","non-literal witness binding carries a literal");
            if((witness.source_kind==PredicateWitnessSourceKindV1::Observation&&(!witness.source_ordinal||*witness.source_ordinal>=bundle.observations.size()))
                ||(witness.source_kind==PredicateWitnessSourceKindV1::Baseline&&(!witness.source_ordinal||*witness.source_ordinal>=bundle.baselines.size()))
                ||(witness.source_kind==PredicateWitnessSourceKindV1::Parameter&&(!witness.source_ordinal||*witness.source_ordinal>=bundle.parameters.size())))
                return Invalid("predicate.witness_binding_invalid","predicate witness references unavailable evidence");
            if(witness.source_kind==PredicateWitnessSourceKindV1::Observation){const auto& observation=bundle.observations[*witness.source_ordinal];if(observation.semantic_hook_id!=check.use.semantic_point_id||observation.value_type!=witness.value_type)return Invalid("predicate.witness_binding_invalid","live observation witness must be acquired at the check hook with one exact type");}
            if(witness.source_kind==PredicateWitnessSourceKindV1::Baseline&&bundle.observations[bundle.baselines[*witness.source_ordinal].observation_ordinal].value_type!=witness.value_type)
                return Invalid("predicate.witness_binding_invalid","baseline witness type does not match its observation");
            if(witness.source_kind==PredicateWitnessSourceKindV1::Parameter&&bundle.parameters[*witness.source_ordinal].value_type!=witness.value_type)
                return Invalid("predicate.witness_binding_invalid","parameter witness type does not match its declaration");
            if(witness.source_kind==PredicateWitnessSourceKindV1::HookReceipt){const bool u32=witness.source_field=="pc"&&witness.value_type==TypeRef::Builtin(BuiltinType::U32);const bool u64=(witness.source_field=="movie_input_count"||witness.source_field=="workset_epoch"||witness.source_field=="vi_count")&&witness.value_type==TypeRef::Builtin(BuiltinType::U64);if(!u32&&!u64)return Invalid("predicate.witness_binding_invalid","hook-receipt witness field or exact type is unavailable");}
        }
        if(check.guard_predicate_definition_revision_id){const auto guard=definitions.find(*check.guard_predicate_definition_revision_id);if(guard==definitions.end()||guard->second->witnesses!=definition->second->witnesses)return Invalid("predicate.occurrence_invalid","GuardOnce requires a published guard with the same exact witness signature");}
        if(active.contains(check.ordinal)){
            ProgramModule module;const auto lowered=LowerPredicate(*definition->second,check.use,module);
            if(!lowered)return Invalid(lowered.diagnostics.empty()?"predicate.lowering_failed":lowered.diagnostics.front().code,
                lowered.diagnostics.empty()?"predicate could not be lowered":lowered.diagnostics.front().message);
        }
    }
    if(!binding.active_check_ordinals.empty()&&binding.active_check_ordinals.back()>=bundle.checks.size())
        return Invalid("predicate.active_check_mismatch","predicate binding activates an unknown check");
    return {true,{},{}};
}

bool EncodePredicateBundleExecutionPackageV1(
    const PredicateBundleExecutionPackageV1& package,
    std::vector<std::uint8_t>& output,
    std::string* diagnostic)
{
    const auto validation=ValidatePredicateBundlePackageV1(package);
    if(!validation){SetDiagnostic(diagnostic,validation.code+": "+validation.message);return false;}
    Writer writer;writer.U32(PredicateBundleWireVersionV1);EncodeHook(writer,package.hook_contract,true);EncodeBundle(writer,package.bundle,true);EncodeBinding(writer,package.binding,true);
    output=std::move(writer.bytes);SetDiagnostic(diagnostic,{});return true;
}

bool DecodePredicateBundleExecutionPackageV1(
    std::span<const std::uint8_t> input,
    PredicateBundleExecutionPackageV1& output,
    std::string* diagnostic)
{
    Reader reader(input);std::uint32_t version=0;PredicateBundleExecutionPackageV1 decoded;
    if(!reader.U32(version)||version!=PredicateBundleWireVersionV1||!DecodeHook(reader,decoded.hook_contract)
        ||!DecodeBundle(reader,decoded.bundle)||!DecodeBinding(reader,decoded.binding)||!reader.Done()){
        SetDiagnostic(diagnostic,"predicate bundle execution package is malformed");return false;}
    const auto validation=ValidatePredicateBundlePackageV1(decoded);
    if(!validation){SetDiagnostic(diagnostic,validation.code+": "+validation.message);return false;}
    output=std::move(decoded);SetDiagnostic(diagnostic,{});return true;
}

} // namespace savor::runtime::predicates
