#include "SemanticObservationComposition.h"

#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "Runner/Runtime/StopPoints/StopPointTypes.h"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace savor::runtime::program::composition {
namespace {

using detail::ModuleFragmentBuilder;

std::string PointLabel(const SemanticAwaitDefinition& definition)
{
    std::string text = "points[";
    for (std::size_t index = 0; index < definition.alternatives.size(); ++index)
    {
        if (index != 0)
            text += ',';
        text += definition.alternatives[index].canonical_id;
    }
    text += ']';
    return text;
}

InstructionTarget ActionTarget(const ExactDependencyIdentity& dependency)
{
    return {
        .kind = InstructionTargetKind::Action,
        .dependency = dependency,
    };
}

class StaticConfigWriter final
{
public:
    explicit StaticConfigWriter(std::array<char, 4> magic)
    {
        for (const char value : magic)
            bytes_.push_back(static_cast<Byte>(value));
    }

    void U8(std::uint8_t value) { bytes_.push_back(value); }
    void Bool(bool value) { U8(value ? 1u : 0u); }
    void U32(std::uint32_t value)
    {
        for (unsigned shift = 0; shift != 32; shift += 8)
            bytes_.push_back(
                static_cast<Byte>((value >> shift) & 0xffu));
    }
    void U64(std::uint64_t value)
    {
        for (unsigned shift = 0; shift != 64; shift += 8)
            bytes_.push_back(
                static_cast<Byte>((value >> shift) & 0xffu));
    }
    void String(std::string_view value)
    {
        if (value.size() >
            std::numeric_limits<std::uint32_t>::max())
        {
            throw std::length_error("static config text is too large");
        }
        U32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void Hash(const ContentHash256& value)
    {
        bytes_.insert(
            bytes_.end(),
            value.bytes.begin(),
            value.bytes.end());
    }
    void Type(const TypeRef& value)
    {
        Bool(value.is_named());
        if (!value.is_named())
        {
            U8(static_cast<std::uint8_t>(value.builtin));
            return;
        }
        String(value.named->canonical_id);
        U32(value.named->version);
        Hash(value.named->schema_hash);
    }

    [[nodiscard]] std::vector<Byte> Finish() &&
    {
        return std::move(bytes_);
    }

private:
    std::vector<Byte> bytes_;
};

class SemanticPointSetReader final
{
public:
    explicit SemanticPointSetReader(std::span<const Byte> bytes)
        : bytes_(bytes)
    {
        constexpr std::array<char, 4> magic{'S', 'P', 'S', '1'};
        if (bytes_.size() < magic.size())
            return;
        for (std::size_t index = 0; index < magic.size(); ++index)
        {
            if (bytes_[index] != static_cast<Byte>(magic[index]))
                return;
        }
        offset_ = magic.size();
        valid_ = true;
    }

    bool U8(std::uint8_t& value)
    {
        if (!Take(1)) return false;
        value = bytes_[offset_++];
        return true;
    }
    bool Bool(bool& value)
    {
        std::uint8_t encoded = 0;
        if (!U8(encoded) || encoded > 1) return false;
        value = encoded != 0;
        return true;
    }
    bool U32(std::uint32_t& value)
    {
        if (!Take(4)) return false;
        value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8)
            value |= static_cast<std::uint32_t>(bytes_[offset_++]) << shift;
        return true;
    }
    bool String(std::string& value, std::size_t maximum = 4096)
    {
        std::uint32_t size = 0;
        if (!U32(size) || size > maximum || !Take(size)) return false;
        value.assign(
            reinterpret_cast<const char*>(bytes_.data() + offset_),
            size);
        offset_ += size;
        return !value.empty() && std::ranges::none_of(
            value,
            [](unsigned char character) { return character == 0; });
    }
    bool Hash(ContentHash256& value)
    {
        if (!Take(value.bytes.size())) return false;
        std::ranges::copy(
            bytes_.subspan(offset_, value.bytes.size()),
            value.bytes.begin());
        offset_ += value.bytes.size();
        return !value.empty();
    }
    bool Type(TypeRef& value)
    {
        bool named = false;
        if (!Bool(named)) return false;
        if (!named)
        {
            std::uint8_t builtin = 0;
            if (!U8(builtin) || builtin >
                    static_cast<std::uint8_t>(BuiltinType::F64))
                return false;
            value = TypeRef::Builtin(static_cast<BuiltinType>(builtin));
            return true;
        }
        SchemaIdentity identity;
        if (!String(identity.canonical_id) ||
            !U32(identity.version) || identity.version == 0 ||
            !Hash(identity.schema_hash))
            return false;
        value = TypeRef::Named(std::move(identity));
        return true;
    }
    [[nodiscard]] bool done() const noexcept
    {
        return valid_ && offset_ == bytes_.size();
    }

private:
    [[nodiscard]] bool Take(std::size_t count) const noexcept
    {
        return valid_ && count <= bytes_.size() - offset_;
    }
    std::span<const Byte> bytes_;
    std::size_t offset_ = 0;
    bool valid_ = false;
};

const SemanticPointDescriptor* FindRegisteredPoint(
    const CapabilityPackIdentity& pack,
    std::string_view canonical_id)
{
    static const capabilities::SourceCapabilityPackCatalog catalog =
        capabilities::BuildSourceCapabilityPackCatalog();
    const auto manifest = std::ranges::find(
        catalog.manifests,
        pack,
        &CapabilityPackManifest::identity);
    if (manifest == catalog.manifests.end()) return nullptr;
    const auto point = std::ranges::find(
        manifest->semantic_points,
        canonical_id,
        &SemanticPointDescriptor::canonical_id);
    return point == manifest->semantic_points.end() ? nullptr : &*point;
}

const CpuEvaluatorDescriptor* FindRegisteredEvaluator(
    std::string_view canonical_id)
{
    static const capabilities::SourceCapabilityPackCatalog catalog =
        capabilities::BuildSourceCapabilityPackCatalog();
    const CpuEvaluatorDescriptor* result = nullptr;
    for (const CapabilityPackManifest& manifest : catalog.manifests)
    {
        const auto found = std::ranges::find(
            manifest.cpu_evaluators,
            canonical_id,
            &CpuEvaluatorDescriptor::canonical_id);
        if (found == manifest.cpu_evaluators.end()) continue;
        if (result != nullptr) return nullptr;
        result = &*found;
    }
    return result;
}

void AddCanonicalActionSchemaImports(
    ModuleFragmentBuilder& builder,
    CanonicalAction action)
{
    for (const auto& schema :
         CanonicalActionTypeSchemaClosure(action))
    {
        builder.AddTypeImport(schema);
    }
}

std::optional<ProgramValueId> AddStaticConfig(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    CanonicalRuntimeSchema schema,
    std::vector<Byte> bytes,
    std::string selector,
    ProgramScopeId scope)
{
    const TypeRef type = CanonicalRuntimeType(schema);
    return builder.AddInstruction(
        function,
        block,
        InstructionOpcode::Constant,
        type,
        {},
        {},
        std::move(selector),
        LiteralValue{
            .type = type,
            .payload = std::move(bytes),
        },
        scope);
}

std::optional<ProgramValueId> AddOptional(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    CanonicalRuntimeSchema schema,
    std::optional<ProgramValueId> value,
    std::string selector,
    ProgramScopeId scope)
{
    std::vector<ProgramValueId> operands;
    if (value)
        operands.push_back(*value);
    return builder.AddInstruction(
        function,
        block,
        InstructionOpcode::OptionalConstruct,
        CanonicalRuntimeType(schema),
        operands,
        {},
        std::move(selector),
        std::nullopt,
        scope);
}

std::optional<ProgramValueId> AddRequest(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    CanonicalAction action,
    std::span<const ProgramValueId> fields,
    std::string selector,
    ProgramScopeId scope)
{
    return builder.AddInstruction(
        function,
        block,
        InstructionOpcode::RecordConstruct,
        CanonicalActionInputType(action),
        fields,
        {},
        std::move(selector),
        std::nullopt,
        scope);
}

std::vector<Byte> EncodeSemanticPointSetBytes(
    std::span<const SemanticPointReference> alternatives,
    std::span<const HitTimeSampleRequirement> hit_time_samples)
{
    StaticConfigWriter writer({'S', 'P', 'S', '1'});
    writer.U32(static_cast<std::uint32_t>(
        alternatives.size()));
    for (const auto& point : alternatives)
    {
        writer.String(point.capability_pack.canonical_id);
        writer.U32(point.capability_pack.version);
        writer.Hash(point.capability_pack.manifest_hash);
        writer.String(point.canonical_id);
        writer.U8(static_cast<std::uint8_t>(point.kind));
        writer.U32(point.physical_pc);
    }
    writer.U32(static_cast<std::uint32_t>(
        hit_time_samples.size()));
    for (const auto& sample : hit_time_samples)
    {
        writer.String(sample.canonical_id);
        writer.Type(sample.result_type);
        writer.U32(sample.maximum_bytes);
        writer.Bool(sample.required);
    }
    return std::move(writer).Finish();
}

std::vector<Byte> ContinueConfig(
    const SemanticAwaitDefinition& definition)
{
    StaticConfigWriter writer({'C', 'U', 'C', '1'});
    writer.U8(static_cast<std::uint8_t>(
        definition.current_point));
    writer.Bool(definition.suppress_immediate_reentry);
    writer.U8(static_cast<std::uint8_t>(
        definition.movie_policy));
    // Default Slice 5 execution policies: preserve throttle and fail closed
    // on an unregistered interruption.
    writer.U8(0);
    writer.U8(0);
    return std::move(writer).Finish();
}

std::vector<Byte> AdvanceConfig(
    ObservationAdvanceKind kind)
{
    StaticConfigWriter writer({'E', 'A', 'C', '1'});
    writer.U8(static_cast<std::uint8_t>(kind));
    // Fail if a previously active movie ends; preserve throttle; reject
    // interruptions.
    writer.U8(1);
    writer.U8(0);
    writer.U8(0);
    return std::move(writer).Finish();
}

std::vector<Byte> ObservationConfig(
    std::string_view observation_id,
    ObservationRequirement requirement,
    bool coherent_query,
    std::uint8_t maximum_dereference_depth = 0)
{
    StaticConfigWriter writer({'O', 'S', 'C', '1'});
    writer.String(observation_id);
    writer.U8(static_cast<std::uint8_t>(requirement));
    writer.Bool(coherent_query);
    writer.U8(maximum_dereference_depth);
    return std::move(writer).Finish();
}

std::optional<ProgramValueId> AddCanonicalGuestRequest(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    CanonicalAction action,
    std::optional<ProgramValueId> stop_receipt,
    std::optional<ProgramValueId> address,
    std::vector<Byte> config,
    std::string selector,
    ProgramScopeId scope)
{
    const auto optional_stop = AddOptional(
        builder,
        function,
        block,
        CanonicalRuntimeSchema::OptionalContinueUntilResult,
        stop_receipt,
        selector + "/stop-receipt",
        scope);
    const auto static_config = AddStaticConfig(
        builder,
        function,
        block,
        CanonicalRuntimeSchema::ObservationStaticConfig,
        std::move(config),
        selector + "/static-config",
        scope);
    if (!optional_stop || !static_config)
        return std::nullopt;
    std::vector<ProgramValueId> fields{*optional_stop};
    if (action != CanonicalAction::GuestRunCoherentQuery)
    {
        if (!address)
            return std::nullopt;
        fields.push_back(*address);
    }
    fields.push_back(*static_config);
    return AddRequest(
        builder,
        function,
        block,
        action,
        fields,
        std::move(selector),
        scope);
}

std::optional<ProgramValueId> AddLiteral(
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    LiteralValue value,
    std::string selector,
    ProgramScopeId scope)
{
    return builder.AddInstruction(
        function,
        block,
        InstructionOpcode::Constant,
        value.type,
        {},
        {},
        std::move(selector),
        std::move(value),
        scope);
}

const AddressExpression* FindAddress(
    const SemanticObservationComposition& definition,
    const std::string& id)
{
    const auto found = std::ranges::find(
        definition.address_expressions,
        id,
        &AddressExpression::canonical_id);
    return found == definition.address_expressions.end()
        ? nullptr
        : &*found;
}

const ObservationDefinition* FindObservation(
    const SemanticObservationComposition& definition,
    const std::string& id)
{
    const auto found = std::ranges::find(
        definition.observations,
        id,
        &ObservationDefinition::canonical_id);
    return found == definition.observations.end()
        ? nullptr
        : &*found;
}

bool Permits(
    const ObservationDefinition& definition,
    ObservationAcquisitionMode mode)
{
    return std::ranges::find(definition.permitted_modes, mode) !=
        definition.permitted_modes.end();
}

std::optional<CompositionResult> Validate(
    const SemanticObservationComposition& definition)
{
    if (definition.canonical_id.empty() || definition.revision == 0)
        return detail::Fail(
            "semantic.invalid_identity",
            "semantic observation identity and revision are required");
    if (definition.await.alternatives.empty())
        return detail::Fail(
            "semantic.missing_point",
            "semantic await requires at least one point alternative");
    if (definition.await.continue_until_action.canonical_id.empty())
    {
        return detail::Fail(
            "semantic.missing_runtime_action",
            "semantic await requires the exact continue action");
    }
    if (definition.await.continue_until_action !=
            CanonicalActionIdentity(
                CanonicalAction::ExecutionContinueUntil) ||
        definition.await.receipt_type !=
            CanonicalActionOutputType(
                CanonicalAction::ExecutionContinueUntil))
    {
        return detail::Fail(
            "semantic.noncanonical_runtime_contract",
            "semantic await requires the exact canonical continue-until contract");
    }

    std::set<std::string> point_ids;
    for (const auto& point : definition.await.alternatives)
    {
        if (point.canonical_id.empty() ||
            !point_ids.insert(point.canonical_id).second)
        {
            return detail::Fail(
                "semantic.invalid_point",
                "semantic point identities must be nonempty and unique");
        }
        if (point.kind == SemanticPointKind::ProgramCounter &&
            point.physical_pc == 0)
        {
            return detail::Fail(
                "semantic.zero_pc",
                "program-counter semantic points require a nonzero PC");
        }
    }

    std::set<std::string> sample_ids;
    for (const auto& sample : definition.await.hit_time_samples)
    {
        if (sample.canonical_id.empty() ||
            sample.maximum_bytes == 0 ||
            sample.maximum_bytes > 4096 ||
            sample.projection_reducer.canonical_id.empty() ||
            !sample_ids.insert(sample.canonical_id).second)
        {
            return detail::Fail(
                "semantic.invalid_sample",
                "hit-time samples must be unique and bounded to 1..4096 bytes");
        }
    }

    std::set<std::string> address_ids;
    for (const auto& address : definition.address_expressions)
    {
        if (address.canonical_id.empty() ||
            !address_ids.insert(address.canonical_id).second)
        {
            return detail::Fail(
                "semantic.invalid_address",
                "address expression identities must be nonempty and unique");
        }
        if ((address.kind == AddressExpressionKind::CheckedOffset ||
             address.kind == AddressExpressionKind::CheckedDereference) &&
            (!address.base_expression ||
             !address_ids.contains(*address.base_expression)))
        {
            return detail::Fail(
                "semantic.missing_address_base",
                "derived address expressions require an earlier registered base");
        }
        if (address.kind == AddressExpressionKind::CheckedDereference &&
            (address.maximum_dereference_depth == 0 ||
             address.maximum_dereference_depth > 8 ||
             !address.dereference_action ||
             !FindCanonicalAction(*address.dereference_action)))
        {
            return detail::Fail(
                "semantic.invalid_dereference",
                "checked dereferences require an exact action and depth 1..8");
        }
    }

    std::set<std::string> observation_ids;
    for (const auto& observation : definition.observations)
    {
        if (observation.canonical_id.empty() ||
            observation.permitted_modes.empty() ||
            !observation_ids.insert(observation.canonical_id).second)
        {
            return detail::Fail(
                "semantic.invalid_observation",
                "observation identities and permitted modes are required");
        }
        if (observation.address_expression &&
            FindAddress(definition, *observation.address_expression) == nullptr)
        {
            return detail::Fail(
                "semantic.unknown_address",
                "observation references an unknown address expression");
        }
        if (Permits(observation, ObservationAcquisitionMode::HitTimeSample) &&
            (!observation.hit_time_sample ||
             !sample_ids.contains(*observation.hit_time_sample)))
        {
            return detail::Fail(
                "semantic.unknown_sample",
                "hit-time observation references an undeclared sample");
        }
        if (Permits(observation, ObservationAcquisitionMode::PausedAtPoint) &&
            !observation.paused_action)
        {
            return detail::Fail(
                "semantic.missing_paused_action",
                "paused observation requires an exact read or query action");
        }
        if (Permits(
                observation,
                ObservationAcquisitionMode::PausedAtPoint) &&
            observation.paused_action)
        {
            const auto canonical =
                FindCanonicalAction(*observation.paused_action);
            if (canonical &&
                CanonicalActionOutputType(*canonical) !=
                    observation.result_type)
            {
                return detail::Fail(
                    "semantic.query_result_type_mismatch",
                    "canonical paused observation result must match the action descriptor");
            }
            if (!canonical &&
                (!observation.paused_action_pack ||
                 !observation.paused_request_type ||
                 !observation.paused_request_type->is_named() ||
                 observation.paused_request_fields.empty()))
            {
                return detail::Fail(
                    "semantic.missing_query_request_contract",
                    "capability-pack observations require an exact named request record and ordered field projections");
            }
            if (canonical &&
                (observation.paused_action_pack ||
                 observation.paused_request_type ||
                 !observation.paused_request_fields.empty()))
            {
                return detail::Fail(
                    "semantic.redundant_query_request_contract",
                    "canonical guest actions use their catalog request record");
            }
            std::set<std::string> request_fields;
            for (const auto& binding :
                 observation.paused_request_fields)
            {
                if (binding.field_name.empty() ||
                    !request_fields.insert(
                        binding.field_name).second ||
                    (binding.source ==
                         ObservationDefinition::
                             RequestValueSource::StopReceiptField &&
                     binding.source_field.empty()) ||
                    (binding.source ==
                         ObservationDefinition::
                             RequestValueSource::Address &&
                     !observation.address_expression))
                {
                    return detail::Fail(
                        "semantic.invalid_query_request_contract",
                        "query request fields require unique names and available typed sources");
                }
            }
        }
    }

    std::set<std::string> use_ids;
    TypeRef expected_output = definition.await.receipt_type;
    for (const auto& use : definition.ordered_uses)
    {
        const auto* observation = FindObservation(
            definition,
            use.definition_id);
        if (use.canonical_id.empty() ||
            !use_ids.insert(use.canonical_id).second ||
            observation == nullptr ||
            !Permits(*observation, use.mode))
        {
            return detail::Fail(
                "semantic.invalid_use",
                "observation uses must be unique and select a permitted definition");
        }
        if (use.baseline != BaselineUpdatePolicy::None &&
            use.baseline_name.empty())
        {
            return detail::Fail(
                "semantic.invalid_baseline",
                "baseline update policy requires a named baseline");
        }
        if (use.advance_before_observation != ObservationAdvanceKind::None &&
            (!use.advance_action || use.advance_count == 0))
        {
            return detail::Fail(
                "semantic.invalid_explicit_advance",
                "post-frame observation requires an exact bounded frame-step action");
        }
        if (use.advance_before_observation != ObservationAdvanceKind::None &&
            use.mode != ObservationAcquisitionMode::PausedAtPoint)
        {
            return detail::Fail(
                "semantic.invalid_post_step_mode",
                "explicit post-step observations must acquire evidence while paused");
        }
        expected_output = observation->result_type;
    }
    if (definition.output_type != expected_output)
    {
        return detail::Fail(
            "semantic.output_type_mismatch",
            "semantic output type must match the final ordered observation or receipt");
    }
    return std::nullopt;
}

std::optional<ProgramValueId> LowerAddress(
    const AddressExpression& address,
    ProgramValueId receipt,
    const std::map<std::string, ProgramValueId>& lowered_addresses,
    ModuleFragmentBuilder& builder,
    ProgramFunction& function,
    BasicBlock& block,
    ProgramScopeId scope)
{
    switch (address.kind)
    {
    case AddressExpressionKind::RegisteredSymbol:
    case AddressExpressionKind::CompatibilityPinnedAddress:
        return AddLiteral(
            builder,
            function,
            block,
            {
                .type = address.result_type,
                .payload = static_cast<std::uint64_t>(address.absolute_address),
            },
            "address/" + address.canonical_id + "/" + address.symbol_or_field,
            scope);
    case AddressExpressionKind::ReceiptField:
        return builder.AddInstruction(
            function,
            block,
            InstructionOpcode::RecordProject,
            address.result_type,
            std::array{receipt},
            {},
            address.symbol_or_field,
            std::nullopt,
            scope);
    case AddressExpressionKind::CheckedOffset:
    {
        const auto base = lowered_addresses.at(*address.base_expression);
        const auto magnitude = address.checked_offset < 0
            ? static_cast<std::uint64_t>(
                -(address.checked_offset + 1)) + 1u
            : static_cast<std::uint64_t>(address.checked_offset);
        const auto offset = AddLiteral(
            builder,
            function,
            block,
            {
                .type = address.result_type,
                .payload = magnitude,
            },
            "address-offset/" + address.canonical_id,
            scope);
        if (!offset) return std::nullopt;
        return builder.AddInstruction(
            function,
            block,
            address.checked_offset < 0
                ? InstructionOpcode::SubtractChecked
                : InstructionOpcode::AddChecked,
            address.result_type,
            std::array{base, *offset},
            {},
            "checked-offset/" + address.canonical_id,
            std::nullopt,
            scope);
    }
    case AddressExpressionKind::CheckedDereference:
    {
        const auto base = lowered_addresses.at(*address.base_expression);
        const auto action =
            *FindCanonicalAction(*address.dereference_action);
        builder.AddActionImport(*address.dereference_action);
        AddCanonicalActionSchemaImports(builder, action);
        const auto request = AddCanonicalGuestRequest(
            builder,
            function,
            block,
            action,
            std::nullopt,
            base,
            ObservationConfig(
                address.canonical_id,
                ObservationRequirement::Required,
                false,
                address.maximum_dereference_depth),
            "checked-dereference/" + address.canonical_id +
                "/request",
            scope);
        if (!request)
            return std::nullopt;
        return builder.AddInstruction(
            function,
            block,
            InstructionOpcode::AwaitAction,
            address.result_type,
            std::array{*request},
            ActionTarget(*address.dereference_action),
            "checked-dereference/" + address.canonical_id,
            std::nullopt,
            {});
    }
    }
    return std::nullopt;
}

} // namespace

std::vector<Byte> EncodeSemanticPointSetV1(
    std::span<const SemanticPointReference> alternatives,
    std::span<const HitTimeSampleRequirement> hit_time_samples)
{
    return EncodeSemanticPointSetBytes(alternatives, hit_time_samples);
}

SemanticPointSetDecodeResultV1 DecodeSemanticPointSetV1(
    std::span<const Byte> bytes)
{
    constexpr std::uint32_t maximum_alternatives = 128;
    SemanticPointSetReader reader(bytes);
    std::uint32_t alternative_count = 0;
    if (!reader.U32(alternative_count) || alternative_count == 0 ||
        alternative_count > maximum_alternatives)
    {
        return {{}, "SPS1 has an invalid alternative count"};
    }

    ResolvedSemanticPointSetV1 resolved;
    resolved.program_counters.reserve(alternative_count);
    std::set<std::pair<std::string, std::string>> point_ids;
    for (std::uint32_t index = 0; index < alternative_count; ++index)
    {
        CapabilityPackIdentity pack;
        std::string point_id;
        std::uint8_t kind = 0;
        std::uint32_t pc = 0;
        if (!reader.String(pack.canonical_id) ||
            !reader.U32(pack.version) || pack.version == 0 ||
            !reader.Hash(pack.manifest_hash) ||
            !reader.String(point_id) || !reader.U8(kind) ||
            !reader.U32(pc))
        {
            return {{}, "SPS1 contains a malformed semantic point"};
        }
        const SemanticPointDescriptor* point =
            FindRegisteredPoint(pack, point_id);
        if (point == nullptr ||
            kind != static_cast<std::uint8_t>(
                SemanticPointKind::ProgramCounter) ||
            point->kind != SemanticPointKind::ProgramCounter ||
            point->pc == 0 || point->pc != pc ||
            !point_ids.emplace(pack.canonical_id, point_id).second)
        {
            return {{}, "SPS1 semantic point is not an exact unique registered PC point"};
        }
        resolved.program_counters.push_back(pc);
    }

    std::uint32_t sample_count = 0;
    if (!reader.U32(sample_count) ||
        sample_count > kMaxRoutedHitSamples)
    {
        return {{}, "SPS1 has an invalid hit-time sample count"};
    }
    resolved.hit_time_sample_descriptor_ids.reserve(sample_count);
    std::set<std::string> sampler_ids;
    for (std::uint32_t index = 0; index < sample_count; ++index)
    {
        std::string evaluator_id;
        TypeRef result_type;
        std::uint32_t maximum_bytes = 0;
        bool required = false;
        if (!reader.String(evaluator_id) || !reader.Type(result_type) ||
            !reader.U32(maximum_bytes) || !reader.Bool(required))
        {
            return {{}, "SPS1 contains a malformed hit-time sampler"};
        }
        const CpuEvaluatorDescriptor* evaluator =
            FindRegisteredEvaluator(evaluator_id);
        if (evaluator == nullptr ||
            evaluator->routed_sample_descriptor_id == 0 ||
            evaluator->result_type != result_type ||
            maximum_bytes == 0 ||
            maximum_bytes > evaluator->maximum_output_bytes ||
            !sampler_ids.emplace(evaluator_id).second)
        {
            return {{}, "SPS1 contains an unregistered or incompatible hit-time sampler"};
        }
        resolved.hit_time_sample_descriptor_ids.push_back(
            evaluator->routed_sample_descriptor_id);
        (void)required;
    }
    if (!reader.done())
        return {{}, "SPS1 is truncated or has trailing data"};
    return {std::move(resolved), {}};
}

CompositionResult LowerSemanticObservation(
    const SemanticObservationComposition& definition,
    ProgramModule& module)
{
    if (const auto invalid = Validate(definition))
        return *invalid;

    ProgramModule candidate = module;
    ModuleFragmentBuilder builder(
        candidate,
        definition.source_name.empty()
            ? definition.canonical_id
            : definition.source_name,
        std::format(
            "semantic-observation/{}/{}",
            definition.canonical_id,
            definition.revision));

    for (const auto& point : definition.await.alternatives)
        builder.AddCapabilityImport(point.capability_pack);
    // Every semantic await lowers through canonical stop-point and execution
    // actions. Entrypoints must declare their providing pack explicitly even
    // when a game-specific point pack also depends on it.
    builder.AddCapabilityImport(CanonicalRuntimePackIdentity());
    builder.AddActionImport(definition.await.continue_until_action);
    AddCanonicalActionSchemaImports(
        builder,
        CanonicalAction::ExecutionContinueUntil);
    builder.AddTypeImport(definition.output_type);
    builder.AddTypeImport(definition.await.receipt_type);
    for (const auto& address : definition.address_expressions)
        builder.AddTypeImport(address.result_type);
    for (const auto& observation : definition.observations)
        builder.AddTypeImport(observation.result_type);
    for (const auto& sample : definition.await.hit_time_samples)
    {
        builder.AddTypeImport(sample.result_type);
        builder.AddReducerImport(sample.projection_reducer);
    }

    auto& function = builder.AddFunction(
        "observe." + definition.canonical_id,
        {},
        definition.output_type);
    auto& block = builder.AddBlock(function);
    const auto scope = builder.NewScope();

    (void)builder.AddInstruction(
        function,
        block,
        InstructionOpcode::EnterScope,
        std::nullopt,
        {},
        {},
        "scope/semantic-await",
        std::nullopt,
        scope);

    const auto group_config = AddStaticConfig(
        builder,
        function,
        block,
        CanonicalRuntimeSchema::SemanticPointSet,
        EncodeSemanticPointSetBytes(
            definition.await.alternatives,
            definition.await.hit_time_samples),
        "semantic-points/static-config",
        scope);
    const auto no_binding = AddOptional(
        builder,
        function,
        block,
        CanonicalRuntimeSchema::
            OptionalInputExecutionBinding,
        std::nullopt,
        "continue-until/no-input-binding",
        scope);
    const auto no_movie = AddOptional(
        builder,
        function,
        block,
        CanonicalRuntimeSchema::OptionalMoviePlaybackSession,
        std::nullopt,
        "continue-until/no-movie",
        scope);
    const auto no_expected_count = AddOptional(
        builder,
        function,
        block,
        CanonicalRuntimeSchema::OptionalMovieInputCount,
        std::nullopt,
        "continue-until/no-expected-count",
        scope);
    const auto continue_config = AddStaticConfig(
        builder,
        function,
        block,
        CanonicalRuntimeSchema::ContinueUntilStaticConfig,
        ContinueConfig(definition.await),
        "continue-until/static-config",
        scope);
    const auto continue_request =
        group_config && no_binding && no_movie && no_expected_count &&
            continue_config
        ? AddRequest(
              builder,
              function,
              block,
              CanonicalAction::ExecutionContinueUntil,
              std::array{
                  *group_config,
            *no_binding,
                  *no_movie,
                  *no_expected_count,
                  *continue_config},
              "continue-until/request",
              scope)
        : std::nullopt;
    if (!continue_request)
        return detail::Fail(
            "semantic.lowering_failed",
            "continue-until request could not be constructed");
    const auto receipt = builder.AddInstruction(
        function,
        block,
        InstructionOpcode::AwaitAction,
        definition.await.receipt_type,
        std::array{*continue_request},
        ActionTarget(definition.await.continue_until_action),
        "continue-until/" + PointLabel(definition.await),
        std::nullopt,
        {});
    if (!receipt)
        return detail::Fail(
            "semantic.lowering_failed",
            "continue-until action did not produce a receipt");

    // Downstream field projections historically addressed the routed stop
    // directly. ContinueUntil/1 now carries it only for Breakpoint results,
    // so extracting this optional both preserves those field contracts and
    // makes non-breakpoint completion fail closed before observation work.
    const auto routed_stop_optional = builder.AddInstruction(
        function,
        block,
        InstructionOpcode::RecordProject,
        CanonicalRuntimeType(
            CanonicalRuntimeSchema::OptionalRoutedStopReceipt),
        std::array{*receipt},
        {},
        "routed_stop",
        std::nullopt,
        scope);
    const auto routed_stop = routed_stop_optional
        ? builder.AddInstruction(
              function,
              block,
              InstructionOpcode::OptionalExtract,
              CanonicalRuntimeType(
                  CanonicalRuntimeSchema::RoutedStopReceipt),
              std::array{*routed_stop_optional},
              {},
              "continue-until/breakpoint-routed-stop",
              std::nullopt,
              scope)
        : std::nullopt;
    if (!routed_stop)
        return detail::Fail(
            "semantic.lowering_failed",
            "continue-until breakpoint receipt could not be extracted");

    std::map<std::string, ProgramValueId> lowered_addresses;
    for (const auto& address : definition.address_expressions)
    {
        const auto value = LowerAddress(
            address,
            *routed_stop,
            lowered_addresses,
            builder,
            function,
            block,
            scope);
        if (!value)
            return detail::Fail(
                "semantic.address_lowering_failed",
                "address expression could not be lowered");
        lowered_addresses.emplace(address.canonical_id, *value);
    }

    std::optional<ProgramValueId> output = receipt;
    std::map<std::string, ProgramValueId> baselines;
    for (const auto& use : definition.ordered_uses)
    {
        const auto& observation = *FindObservation(
            definition,
            use.definition_id);
        if (use.advance_before_observation != ObservationAdvanceKind::None)
        {
            builder.AddActionImport(*use.advance_action);
            constexpr CanonicalAction advance_action =
                CanonicalAction::ExecutionStepFrames;
            if (*use.advance_action !=
                CanonicalActionIdentity(advance_action))
            {
                return detail::Fail(
                    "semantic.noncanonical_advance",
                    "explicit observation advance must select the matching canonical execution action");
            }
            AddCanonicalActionSchemaImports(
                builder,
                advance_action);
            const auto count = AddLiteral(
                builder,
                function,
                block,
                {
                    .type = TypeRef::Builtin(BuiltinType::U64),
                    .payload = use.advance_count,
                },
                "explicit-observation-advance/count",
                scope);
            const auto no_relationship = AddOptional(
                builder,
                function,
                block,
                CanonicalRuntimeSchema::
                    OptionalInputExecutionBinding,
                std::nullopt,
                "explicit-observation-advance/no-input-relationship",
                scope);
            const auto advance_config = AddStaticConfig(
                builder,
                function,
                block,
                CanonicalRuntimeSchema::
                    ExecutionAdvanceStaticConfig,
                AdvanceConfig(
                    use.advance_before_observation),
                "explicit-observation-advance/static-config",
                scope);
            const auto advance_request =
                count && no_relationship && advance_config
                ? AddRequest(
                      builder,
                      function,
                      block,
                      advance_action,
                      std::array{
                          *count,
                          *no_relationship,
                          *advance_config},
                      "explicit-observation-advance/request",
                      scope)
                : std::nullopt;
            if (!advance_request)
                return detail::Fail(
                    "semantic.advance_lowering_failed",
                    "explicit observation advance request could not be constructed");
            (void)builder.AddInstruction(
                function,
                block,
                InstructionOpcode::AwaitAction,
                CanonicalActionOutputType(advance_action),
                std::array{*advance_request},
                ActionTarget(*use.advance_action),
                "explicit-observation-advance/frames/" +
                    use.canonical_id,
                std::nullopt,
                {});
        }
        std::optional<ProgramValueId> observed;
        if (use.mode == ObservationAcquisitionMode::HitTimeSample)
        {
            const auto sample = std::ranges::find(
                definition.await.hit_time_samples,
                *observation.hit_time_sample,
                &HitTimeSampleRequirement::canonical_id);
            if (sample ==
                definition.await.hit_time_samples.end())
            {
                return detail::Fail(
                    "semantic.sample_lowering_failed",
                    "hit-time sample projection is unavailable");
            }
            observed = builder.AddInstruction(
                function,
                block,
                InstructionOpcode::CallReducer,
                observation.result_type,
                std::array{*receipt},
                {
                    .kind = InstructionTargetKind::Reducer,
                    .dependency = sample->projection_reducer,
                },
                "hit-time-sample/" + *observation.hit_time_sample,
                std::nullopt,
                scope);
        }
        else
        {
            builder.AddActionImport(*observation.paused_action);
            std::optional<ProgramValueId> request;
            if (const auto canonical =
                    FindCanonicalAction(
                        *observation.paused_action))
            {
                AddCanonicalActionSchemaImports(
                    builder,
                    *canonical);
                request = AddCanonicalGuestRequest(
                    builder,
                    function,
                    block,
                    *canonical,
                    *receipt,
                    observation.address_expression
                        ? std::optional<ProgramValueId>(
                              lowered_addresses.at(
                                  *observation.address_expression))
                        : std::nullopt,
                    ObservationConfig(
                        observation.canonical_id,
                        use.requirement,
                        observation.coherent_query),
                    "paused-observation/" +
                        use.canonical_id + "/request",
                    scope);
            }
            else
            {
                builder.AddCapabilityImport(
                    *observation.paused_action_pack);
                builder.AddTypeImport(
                    *observation.paused_request_type);
                std::vector<ProgramValueId> fields;
                fields.reserve(
                    observation.paused_request_fields.size());
                for (const auto& binding :
                     observation.paused_request_fields)
                {
                    builder.AddTypeImport(binding.field_type);
                    switch (binding.source)
                    {
                    case ObservationDefinition::
                        RequestValueSource::StopReceipt:
                        fields.push_back(*receipt);
                        break;
                    case ObservationDefinition::
                        RequestValueSource::StopReceiptField:
                    {
                        const auto projected =
                            builder.AddInstruction(
                                function,
                                block,
                                InstructionOpcode::
                                    RecordProject,
                                binding.field_type,
                                std::array{*routed_stop},
                                {},
                                binding.source_field,
                                std::nullopt,
                                scope);
                        if (!projected)
                        {
                            return detail::Fail(
                                "semantic.query_request_lowering_failed",
                                "stop receipt field projection failed");
                        }
                        fields.push_back(*projected);
                        break;
                    }
                    case ObservationDefinition::
                        RequestValueSource::Address:
                        fields.push_back(
                            lowered_addresses.at(
                                *observation.address_expression));
                        break;
                    }
                }
                request = builder.AddInstruction(
                    function,
                    block,
                    InstructionOpcode::RecordConstruct,
                    *observation.paused_request_type,
                    fields,
                    {},
                    "paused-observation/" +
                        use.canonical_id +
                        "/pack-request",
                    std::nullopt,
                    scope);
            }
            if (!request)
            {
                return detail::Fail(
                    "semantic.query_request_lowering_failed",
                    "paused observation request could not be constructed");
            }
            observed = builder.AddInstruction(
                function,
                block,
                InstructionOpcode::AwaitAction,
                observation.result_type,
                std::array{*request},
                ActionTarget(*observation.paused_action),
                (observation.coherent_query
                    ? "coherent-query/"
                    : "paused-observation/") + use.canonical_id +
                    (use.requirement == ObservationRequirement::Required
                        ? "/required"
                        : "/optional"),
                std::nullopt,
                {});
        }
        if (!observed)
            return detail::Fail(
                "semantic.observation_lowering_failed",
                "observation did not produce a typed value");

        if (use.baseline != BaselineUpdatePolicy::None)
        {
            const auto baseline = builder.AddInstruction(
                function,
                block,
                InstructionOpcode::Copy,
                observation.result_type,
                std::array{*observed},
                {},
                std::string("baseline/") +
                    (use.baseline == BaselineUpdatePolicy::First
                        ? "first/"
                        : "latest/") +
                    use.baseline_name,
                std::nullopt,
                scope);
            if (!baseline)
                return detail::Fail(
                    "semantic.baseline_lowering_failed",
                    "baseline update did not produce a value");
            if (use.baseline == BaselineUpdatePolicy::First)
                baselines.try_emplace(use.baseline_name, *baseline);
            else
                baselines.insert_or_assign(use.baseline_name, *baseline);
        }

        if (use.publication != ObservationPublicationPolicy::None)
        {
            const auto emission_schema = ExactSchema(
                definition.canonical_id + "." +
                    use.canonical_id +
                    ".ObservationEmission",
                definition.revision,
                "record ObservationEmission/1(value:" +
                    (observation.result_type.is_named()
                        ? observation.result_type.named
                              ->canonical_id
                        : std::to_string(
                              static_cast<std::uint8_t>(
                                  observation.result_type
                                      .builtin))) +
                    ")");
            builder.AddLocalType({
                .identity = emission_schema,
                .kind = TypeSchemaKind::Record,
                .record_fields = {{
                    "value",
                    observation.result_type,
                }},
            });
            const auto emission =
                builder.AddInstruction(
                    function,
                    block,
                    InstructionOpcode::RecordConstruct,
                    TypeRef::Named(emission_schema),
                    std::array{*observed},
                    {},
                    "publication/record/" +
                        use.canonical_id,
                    std::nullopt,
                    scope);
            (void)builder.AddInstruction(
                function,
                block,
                InstructionOpcode::EmitRecord,
                std::nullopt,
                std::array{*emission},
                {},
                std::string("publication/") +
                    (use.publication ==
                            ObservationPublicationPolicy::AuthoritativeEmission
                        ? "authoritative/"
                        : "telemetry/") +
                    use.canonical_id,
                std::nullopt,
                scope);
        }
        output = observed;
    }

    (void)builder.AddInstruction(
        function,
        block,
        InstructionOpcode::ExitScope,
        std::nullopt,
        {},
        {},
        "scope/semantic-await/release",
        std::nullopt,
        scope);
    Terminator terminal{
        .kind = TerminatorKind::Return,
        .return_value = output,
    };
    builder.SetTerminator(
        function,
        block,
        std::move(terminal),
        "return");

    const auto function_id = function.id;
    module = std::move(candidate);
    return {
        .ok = true,
        .function = function_id,
    };
}

} // namespace savor::runtime::program::composition
