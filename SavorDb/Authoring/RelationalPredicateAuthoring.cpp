#include "SqliteAuthoringDb.h"

#include "../../SavorCore/Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "../../SavorCore/Utils/Hash.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace savor::db {
namespace {

using namespace savor::runtime;
using namespace savor::runtime::program;
using namespace savor::runtime::program::composition;
using namespace savor::runtime::predicates;

struct Statement {
    sqlite3_stmt* value{};
    Statement(sqlite3* db, const char* sql) {
        if (db) (void)sqlite3_prepare_v2(db, sql, -1, &value, nullptr);
    }
    ~Statement() { if (value) sqlite3_finalize(value); }
    explicit operator bool() const noexcept { return value != nullptr; }
};

void Error(sqlite3* db, std::string* out, std::string fallback) {
    if (out) *out = db ? sqlite3_errmsg(db) : std::move(fallback);
}
bool Exec(sqlite3* db, const char* sql, std::string* error) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc == SQLITE_OK) return true;
    if (error) *error = message ? message : sqlite3_errmsg(db);
    sqlite3_free(message); return false;
}
std::int64_t Millis(types::UtcTimePoint value) {
    return value.time_since_epoch().count();
}
std::string Text(sqlite3_stmt* statement, int column) {
    const auto* text = sqlite3_column_text(statement, column);
    return text ? reinterpret_cast<const char*>(text) : std::string{};
}
void BindText(sqlite3_stmt* statement, int index, std::string_view value) {
    sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}
void BindNullableText(sqlite3_stmt* statement, int index, const std::string& value) {
    if (value.empty()) sqlite3_bind_null(statement, index); else BindText(statement, index, value);
}

bool BindType(sqlite3_stmt* statement, int first, const TypeRef& type) {
    if (!type.is_named()) {
        sqlite3_bind_int(statement, first, static_cast<int>(type.builtin));
        sqlite3_bind_null(statement, first + 1);
        sqlite3_bind_null(statement, first + 2);
        sqlite3_bind_null(statement, first + 3);
    } else {
        sqlite3_bind_null(statement, first);
        BindText(statement, first + 1, type.named->canonical_id);
        sqlite3_bind_int64(statement, first + 2, type.named->version);
        BindText(statement, first + 3, type.named->schema_hash.ToHex());
    }
    return true;
}

std::optional<TypeRef> ReadType(sqlite3_stmt* statement, int first) {
    if (sqlite3_column_type(statement, first) != SQLITE_NULL)
        return TypeRef::Builtin(static_cast<BuiltinType>(sqlite3_column_int(statement, first)));
    const auto hash = ContentHash256::FromHex(Text(statement, first + 3));
    if (!hash) return std::nullopt;
    return TypeRef::Named({Text(statement, first + 1),
        static_cast<std::uint32_t>(sqlite3_column_int64(statement, first + 2)), *hash});
}

std::string NodeKind(PredicateExpressionKind kind) {
    static constexpr const char* names[]{"WITNESS","LITERAL","EQUAL","NOT_EQUAL","LESS","LESS_EQUAL","GREATER","GREATER_EQUAL","BOOLEAN_AND","BOOLEAN_OR","BOOLEAN_NOT","ADD","SUBTRACT","MULTIPLY","DIVIDE","REMAINDER","REGISTERED_REDUCER"};
    const auto index = static_cast<std::size_t>(kind);
    return index < std::size(names) ? names[index] : std::string{};
}
std::optional<PredicateExpressionKind> ParseNodeKind(std::string_view value) {
    for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(PredicateExpressionKind::ImportedReducer); ++raw)
        if (NodeKind(static_cast<PredicateExpressionKind>(raw)) == value)
            return static_cast<PredicateExpressionKind>(raw);
    return std::nullopt;
}

bool BindLiteral(sqlite3_stmt* statement, int kind_column, const LiteralValue& value) {
    int integer_column = kind_column + 1;
    int real_column = kind_column + 2;
    int text_column = kind_column + 3;
    int blob_column = kind_column + 4;
    const auto bind_integer = [&](std::string_view kind, std::int64_t integer) {
        BindText(statement, kind_column, kind); sqlite3_bind_int64(statement, integer_column, integer);
    };
    bool ok = true;
    std::visit([&](const auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, bool>) bind_integer("BOOL", payload ? 1 : 0);
        else if constexpr (std::is_same_v<T, std::uint8_t>) bind_integer("U8", payload);
        else if constexpr (std::is_same_v<T, std::uint16_t>) bind_integer("U16", payload);
        else if constexpr (std::is_same_v<T, std::uint32_t>) bind_integer("U32", payload);
        else if constexpr (std::is_same_v<T, std::uint64_t>) {
            if (payload > static_cast<std::uint64_t>(INT64_MAX)) ok = false; else bind_integer("U64", static_cast<std::int64_t>(payload));
        } else if constexpr (std::is_same_v<T, std::int32_t>) bind_integer("I32", payload);
        else if constexpr (std::is_same_v<T, std::int64_t>) bind_integer("I64", payload);
        else if constexpr (std::is_same_v<T, float>) { BindText(statement, kind_column, "F32"); sqlite3_bind_double(statement, real_column, payload); }
        else if constexpr (std::is_same_v<T, double>) { BindText(statement, kind_column, "F64"); sqlite3_bind_double(statement, real_column, payload); }
        else if constexpr (std::is_same_v<T, std::string>) { BindText(statement, kind_column, "TEXT"); BindText(statement, text_column, payload); }
        else if constexpr (std::is_same_v<T, std::vector<Byte>>) { BindText(statement, kind_column, "BYTES"); sqlite3_bind_blob(statement, blob_column, payload.data(), static_cast<int>(payload.size()), SQLITE_TRANSIENT); }
        else if constexpr (std::is_same_v<T, EnumValue>) bind_integer("ENUM", payload.value);
        else ok = false;
    }, value.payload);
    return ok;
}

std::optional<LiteralValue> ReadLiteral(sqlite3_stmt* statement, int kind_column, TypeRef type) {
    const std::string kind = Text(statement, kind_column);
    LiteralValue value{.type = std::move(type)};
    const std::int64_t integer = sqlite3_column_int64(statement, kind_column + 1);
    if (kind == "BOOL") value.payload = integer != 0;
    else if (kind == "U8") value.payload = static_cast<std::uint8_t>(integer);
    else if (kind == "U16") value.payload = static_cast<std::uint16_t>(integer);
    else if (kind == "U32") value.payload = static_cast<std::uint32_t>(integer);
    else if (kind == "U64") value.payload = static_cast<std::uint64_t>(integer);
    else if (kind == "I32") value.payload = static_cast<std::int32_t>(integer);
    else if (kind == "I64") value.payload = integer;
    else if (kind == "F32") value.payload = static_cast<float>(sqlite3_column_double(statement, kind_column + 2));
    else if (kind == "F64") value.payload = sqlite3_column_double(statement, kind_column + 2);
    else if (kind == "TEXT") value.payload = Text(statement, kind_column + 3);
    else if (kind == "BYTES") {
        const auto* data = static_cast<const Byte*>(sqlite3_column_blob(statement, kind_column + 4));
        const int size = sqlite3_column_bytes(statement, kind_column + 4);
        value.payload = std::vector<Byte>(data, data + std::max(0, size));
    } else if (kind == "ENUM" && value.type.is_named()) value.payload = EnumValue{*value.type.named, integer};
    else return std::nullopt;
    return value;
}

std::string DefinitionHash(const PredicateDefinition& definition, std::string* error) {
    ProgramModule module{.identity = {.canonical_id = "authoring.predicate." + definition.canonical_id, .revision = definition.revision}};
    PredicateCheckUse use{.canonical_id = "publish", .semantic_point_id = "authoring.validation"};
    const auto lowered = LowerPredicate(definition, use, module);
    if (!lowered) {
        if (error) *error = lowered.diagnostics.empty() ? "predicate definition is invalid" : lowered.diagnostics.front().message;
        return {};
    }
    module.identity.module_hash = ComputeProgramModuleHashV1(module);
    return module.identity.module_hash.ToHex();
}

bool DefinitionExistsPublished(sqlite3* db, std::int64_t revision_id) {
    Statement statement(db, "SELECT 1 FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=? AND revision_state='PUBLISHED'");
    if (!statement) return false;
    sqlite3_bind_int64(statement.value, 1, revision_id);
    return sqlite3_step(statement.value) == SQLITE_ROW;
}

} // namespace

bool SqliteAuthoringDb::SavePredicateDefinitionDraftV2(
    const SavePredicateDefinitionDraftV2Command& command,
    std::int64_t* revision_id_out,
    std::string* error_out) {
    if (!db_ || command.stable_key.empty() || command.name.empty() ||
        command.stable_key != command.definition.canonical_id ||
        command.definition.canonical_id.empty() || command.definition.revision == 0 ||
        DefinitionHash(command.definition, error_out).empty()) {
        if (error_out && error_out->empty()) *error_out = "predicate definition draft is invalid";
        return false;
    }
    if (!Exec(db_, "BEGIN IMMEDIATE", error_out)) return false;
    const auto rollback = [&]() { (void)sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); };
    std::int64_t definition_id = 0;
    {
        Statement parent(db_, "INSERT INTO au_predicate_definition_v2(stable_key,name,description,created_at_utc) VALUES(?,?,?,?) ON CONFLICT(stable_key) DO UPDATE SET name=excluded.name,description=excluded.description RETURNING predicate_definition_id");
        if (!parent) { rollback(); Error(db_, error_out, "prepare failed"); return false; }
        BindText(parent.value, 1, command.stable_key);
        BindText(parent.value, 2, command.name);
        BindNullableText(parent.value, 3, command.description);
        sqlite3_bind_int64(parent.value, 4, Millis(command.created_at_utc));
        if (sqlite3_step(parent.value) != SQLITE_ROW) {
            rollback(); Error(db_, error_out, "definition parent insert failed"); return false;
        }
        definition_id = sqlite3_column_int64(parent.value, 0);
    }
    std::int64_t revision_id = 0;
    {
        Statement revision(db_, "INSERT INTO au_predicate_definition_revision_v2(predicate_definition_id,revision_number,revision_state,root_node_ordinal,created_at_utc) VALUES(?,COALESCE((SELECT MAX(revision_number)+1 FROM au_predicate_definition_revision_v2 WHERE predicate_definition_id=?),1),'DRAFT',?,?) RETURNING predicate_definition_revision_id");
        if (!revision) { rollback(); Error(db_, error_out, "prepare failed"); return false; }
        sqlite3_bind_int64(revision.value, 1, definition_id);
        sqlite3_bind_int64(revision.value, 2, definition_id);
        sqlite3_bind_int64(revision.value, 3, command.definition.root_expression);
        sqlite3_bind_int64(revision.value, 4, Millis(command.created_at_utc));
        if (sqlite3_step(revision.value) != SQLITE_ROW) {
            rollback(); Error(db_, error_out, "definition revision insert failed"); return false;
        }
        revision_id = sqlite3_column_int64(revision.value, 0);
    }
    for(std::size_t ordinal=0;ordinal<command.definition.witnesses.size();++ordinal){
        Statement insert(db_,"INSERT INTO au_predicate_witness_v2(predicate_definition_revision_id,witness_ordinal,witness_name,value_builtin_type,value_schema_canonical_id,value_schema_revision,value_schema_sha256) VALUES(?,?,?,?,?,?,?)");
        if(!insert){rollback();return false;}sqlite3_bind_int64(insert.value,1,revision_id);sqlite3_bind_int64(insert.value,2,ordinal);BindText(insert.value,3,command.definition.witnesses[ordinal].name);BindType(insert.value,4,command.definition.witnesses[ordinal].value_type);
        if(sqlite3_step(insert.value)!=SQLITE_DONE){rollback();Error(db_,error_out,"witness insert failed");return false;}
    }
    for(std::size_t ordinal=0;ordinal<command.definition.expression.size();++ordinal){
        const auto& node=command.definition.expression[ordinal];
        Statement insert(db_,"INSERT INTO au_predicate_expression_node_v2(predicate_definition_revision_id,node_ordinal,node_kind,result_builtin_type,result_schema_canonical_id,result_schema_revision,result_schema_sha256,witness_ordinal,literal_kind,literal_integer,literal_real,literal_text,literal_blob,reducer_canonical_id,reducer_revision,reducer_sha256,source_label) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        if(!insert){rollback();return false;}sqlite3_bind_int64(insert.value,1,revision_id);sqlite3_bind_int64(insert.value,2,ordinal);BindText(insert.value,3,NodeKind(node.kind));BindType(insert.value,4,node.result_type);
        if(node.witness_index)sqlite3_bind_int64(insert.value,8,*node.witness_index);else sqlite3_bind_null(insert.value,8);
        if(node.literal&&!BindLiteral(insert.value,9,*node.literal)){rollback();if(error_out)*error_out="unsupported literal";return false;}
        if(node.reducer){BindText(insert.value,14,node.reducer->canonical_id);sqlite3_bind_int64(insert.value,15,node.reducer->version);BindText(insert.value,16,node.reducer->signature_hash.ToHex());}
        BindText(insert.value,17,node.source_label);
        if(sqlite3_step(insert.value)!=SQLITE_DONE){rollback();Error(db_,error_out,"expression node insert failed");return false;}
        for(std::size_t edge=0;edge<node.operands.size();++edge){Statement e(db_,"INSERT INTO au_predicate_expression_edge_v2 VALUES(?,?,?,?)");sqlite3_bind_int64(e.value,1,revision_id);sqlite3_bind_int64(e.value,2,ordinal);sqlite3_bind_int64(e.value,3,edge);sqlite3_bind_int64(e.value,4,node.operands[edge]);if(sqlite3_step(e.value)!=SQLITE_DONE){rollback();Error(db_,error_out,"expression edge insert failed");return false;}}
    }
    if(!Exec(db_,"COMMIT",error_out)){rollback();return false;}if(revision_id_out)*revision_id_out=revision_id;return true;
}

std::optional<PredicateDefinitionRevisionV2Snapshot>
SqliteAuthoringDb::GetPredicateDefinitionRevisionV2(std::int64_t revision_id) const {
    if(!db_||revision_id<=0)return std::nullopt;
    Statement root(db_,"SELECT d.predicate_definition_id,d.stable_key,d.name,COALESCE(d.description,''),r.revision_number,r.revision_state,COALESCE(r.content_sha256,''),r.root_node_ordinal FROM au_predicate_definition_revision_v2 r JOIN au_predicate_definition_v2 d ON d.predicate_definition_id=r.predicate_definition_id WHERE r.predicate_definition_revision_id=?");sqlite3_bind_int64(root.value,1,revision_id);if(sqlite3_step(root.value)!=SQLITE_ROW)return std::nullopt;
    PredicateDefinitionRevisionV2Snapshot out{};out.predicate_definition_id=sqlite3_column_int64(root.value,0);out.predicate_definition_revision_id=revision_id;out.stable_key=Text(root.value,1);out.name=Text(root.value,2);out.description=Text(root.value,3);out.definition.canonical_id=out.stable_key;out.definition.revision=sqlite3_column_int(root.value,4);out.revision_state=Text(root.value,5);out.content_sha256=Text(root.value,6);out.definition.root_expression=static_cast<std::size_t>(sqlite3_column_int64(root.value,7));out.definition.source_name="SavorDb.PredicateV2";
    Statement witnesses(db_,"SELECT witness_name,value_builtin_type,value_schema_canonical_id,value_schema_revision,value_schema_sha256 FROM au_predicate_witness_v2 WHERE predicate_definition_revision_id=? ORDER BY witness_ordinal");sqlite3_bind_int64(witnesses.value,1,revision_id);while(sqlite3_step(witnesses.value)==SQLITE_ROW){auto type=ReadType(witnesses.value,1);if(!type)return std::nullopt;out.definition.witnesses.push_back({Text(witnesses.value,0),*type});}
    Statement nodes(db_,"SELECT node_kind,result_builtin_type,result_schema_canonical_id,result_schema_revision,result_schema_sha256,witness_ordinal,literal_kind,literal_integer,literal_real,literal_text,literal_blob,reducer_canonical_id,reducer_revision,reducer_sha256,source_label,node_ordinal FROM au_predicate_expression_node_v2 WHERE predicate_definition_revision_id=? ORDER BY node_ordinal");sqlite3_bind_int64(nodes.value,1,revision_id);while(sqlite3_step(nodes.value)==SQLITE_ROW){PredicateExpressionNode node{};auto kind=ParseNodeKind(Text(nodes.value,0));auto type=ReadType(nodes.value,1);if(!kind||!type)return std::nullopt;node.kind=*kind;node.result_type=*type;if(sqlite3_column_type(nodes.value,5)!=SQLITE_NULL)node.witness_index=sqlite3_column_int64(nodes.value,5);if(sqlite3_column_type(nodes.value,6)!=SQLITE_NULL){node.literal=ReadLiteral(nodes.value,6,*type);if(!node.literal)return std::nullopt;}if(sqlite3_column_type(nodes.value,11)!=SQLITE_NULL){const auto hash=ContentHash256::FromHex(Text(nodes.value,13));if(!hash)return std::nullopt;node.reducer=ExactDependencyIdentity{Text(nodes.value,11),static_cast<std::uint32_t>(sqlite3_column_int64(nodes.value,12)),*hash};}node.source_label=Text(nodes.value,14);const auto ordinal=sqlite3_column_int64(nodes.value,15);Statement edges(db_,"SELECT operand_node_ordinal FROM au_predicate_expression_edge_v2 WHERE predicate_definition_revision_id=? AND node_ordinal=? ORDER BY operand_ordinal");sqlite3_bind_int64(edges.value,1,revision_id);sqlite3_bind_int64(edges.value,2,ordinal);while(sqlite3_step(edges.value)==SQLITE_ROW)node.operands.push_back(sqlite3_column_int64(edges.value,0));out.definition.expression.push_back(std::move(node));}
    return out;
}

bool SqliteAuthoringDb::PublishPredicateDefinitionRevisionV2(
    std::int64_t revision_id, types::UtcTimePoint published_at_utc,
    std::string* error_out) {
    const auto snapshot=GetPredicateDefinitionRevisionV2(revision_id);if(!snapshot||snapshot->revision_state!="DRAFT"){if(error_out)*error_out="predicate definition revision is not a draft";return false;}const auto content=DefinitionHash(snapshot->definition,error_out);if(content.empty())return false;
    Statement update(db_,"UPDATE au_predicate_definition_revision_v2 SET revision_state='PUBLISHED',content_sha256=?,published_at_utc=? WHERE predicate_definition_revision_id=? AND revision_state='DRAFT'");BindText(update.value,1,content);sqlite3_bind_int64(update.value,2,Millis(published_at_utc));sqlite3_bind_int64(update.value,3,revision_id);if(sqlite3_step(update.value)!=SQLITE_DONE||sqlite3_changes(db_)!=1){Error(db_,error_out,"predicate definition publication failed");return false;}return true;
}

namespace {

std::string ObservationKind(PredicateObservationSourceKindV1 value) {
    switch(value){case PredicateObservationSourceKindV1::GuestAddress:return "GUEST_ADDRESS";case PredicateObservationSourceKindV1::RegisteredQuery:return "REGISTERED_QUERY";case PredicateObservationSourceKindV1::HookReceipt:return "HOOK_RECEIPT";case PredicateObservationSourceKindV1::RegisteredReducer:return "REGISTERED_REDUCER";}return {};
}
std::optional<PredicateObservationSourceKindV1> ParseObservationKind(std::string_view value){if(value=="GUEST_ADDRESS")return PredicateObservationSourceKindV1::GuestAddress;if(value=="REGISTERED_QUERY")return PredicateObservationSourceKindV1::RegisteredQuery;if(value=="HOOK_RECEIPT")return PredicateObservationSourceKindV1::HookReceipt;if(value=="REGISTERED_REDUCER")return PredicateObservationSourceKindV1::RegisteredReducer;return std::nullopt;}
std::string BaselinePolicy(PredicateBaselineUpdatePolicyV1 value){return value==PredicateBaselineUpdatePolicyV1::First?"FIRST":"LATEST";}
std::string Occurrence(PredicateOccurrencePolicyV1 value){switch(value){case PredicateOccurrencePolicyV1::First:return "FIRST";case PredicateOccurrencePolicyV1::Every:return "EVERY";case PredicateOccurrencePolicyV1::Ordinal:return "ORDINAL";case PredicateOccurrencePolicyV1::GuardOnce:return "GUARD_ONCE";}return {};}
std::optional<PredicateOccurrencePolicyV1> ParseOccurrence(std::string_view value){if(value=="FIRST")return PredicateOccurrencePolicyV1::First;if(value=="EVERY")return PredicateOccurrencePolicyV1::Every;if(value=="ORDINAL")return PredicateOccurrencePolicyV1::Ordinal;if(value=="GUARD_ONCE")return PredicateOccurrencePolicyV1::GuardOnce;return std::nullopt;}
std::string WitnessSource(PredicateWitnessSourceKindV1 value){switch(value){case PredicateWitnessSourceKindV1::Observation:return "OBSERVATION";case PredicateWitnessSourceKindV1::Baseline:return "BASELINE";case PredicateWitnessSourceKindV1::Parameter:return "PARAMETER";case PredicateWitnessSourceKindV1::HookReceipt:return "HOOK_RECEIPT";case PredicateWitnessSourceKindV1::Literal:return "LITERAL";}return {};}
std::optional<PredicateWitnessSourceKindV1> ParseWitnessSource(std::string_view value){if(value=="OBSERVATION")return PredicateWitnessSourceKindV1::Observation;if(value=="BASELINE")return PredicateWitnessSourceKindV1::Baseline;if(value=="PARAMETER")return PredicateWitnessSourceKindV1::Parameter;if(value=="HOOK_RECEIPT")return PredicateWitnessSourceKindV1::HookReceipt;if(value=="LITERAL")return PredicateWitnessSourceKindV1::Literal;return std::nullopt;}

std::optional<LiteralValue> ZeroLiteral(const TypeRef& type){
    if(type.is_named())return std::nullopt;LiteralValue value{.type=type};switch(type.builtin){case BuiltinType::Unit:value.payload=UnitValue{};break;case BuiltinType::Bool:value.payload=false;break;case BuiltinType::U8:value.payload=std::uint8_t{};break;case BuiltinType::U16:value.payload=std::uint16_t{};break;case BuiltinType::U32:value.payload=std::uint32_t{};break;case BuiltinType::U64:value.payload=std::uint64_t{};break;case BuiltinType::I32:value.payload=std::int32_t{};break;case BuiltinType::I64:value.payload=std::int64_t{};break;case BuiltinType::F32:value.payload=float{};break;case BuiltinType::F64:value.payload=double{};break;}return value;
}

bool ValidateBundleForPublication(ResolvedPredicateBundleV1 bundle,std::string* error){
    bundle.content_sha256=ComputeResolvedPredicateBundleHashV1(bundle);PredicateBundleBindingV1 binding{.bundle_revision_id=bundle.bundle_revision_id,.bundle_content_sha256=bundle.content_sha256};for(const auto& parameter:bundle.parameters){auto value=ZeroLiteral(parameter.value_type);if(!value){if(error)*error="named predicate parameters require an explicit publication validator";return false;}binding.parameter_values.push_back(std::move(*value));}for(const auto& check:bundle.checks)binding.active_check_ordinals.push_back(check.ordinal);binding.structural_active_check_sha256=ComputePredicateActiveCheckSetHashV1(binding.active_check_ordinals);binding.content_sha256=ComputePredicateBundleBindingHashV1(binding);const auto validation=ValidatePredicateBundlePackageV1({BattlePredicateHookContractV1(),std::move(bundle),std::move(binding)});if(!validation){if(error)*error=validation.code+": "+validation.message;return false;}return true;
}

} // namespace

bool SqliteAuthoringDb::SavePredicateBundleDraftV2(
    const SavePredicateBundleDraftV2Command& command,
    std::int64_t* revision_id_out,
    std::string* error_out) {
    if(!db_||command.stable_key.empty()||command.name.empty()||command.bundle.canonical_id.empty()||command.stable_key!=command.bundle.canonical_id){if(error_out)*error_out="predicate bundle draft is invalid";return false;}
    for(const auto& definition:command.bundle.definitions)if(!DefinitionExistsPublished(db_,definition.revision_id)){if(error_out)*error_out="predicate bundle references an unpublished definition";return false;}
    if (!Exec(db_, "BEGIN IMMEDIATE", error_out)) return false;
    const auto rollback = [&]() {
        (void)sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    };
    std::int64_t bundle_id = 0;
    {
        Statement parent(db_, "INSERT INTO au_predicate_bundle_v2(stable_key,name,description,created_at_utc) VALUES(?,?,?,?) ON CONFLICT(stable_key) DO UPDATE SET name=excluded.name,description=excluded.description RETURNING predicate_bundle_id");
        if (!parent) { rollback(); Error(db_, error_out, "prepare failed"); return false; }
        BindText(parent.value, 1, command.stable_key);
        BindText(parent.value, 2, command.name);
        BindNullableText(parent.value, 3, command.description);
        sqlite3_bind_int64(parent.value, 4, Millis(command.created_at_utc));
        if (sqlite3_step(parent.value) != SQLITE_ROW) {
            rollback(); Error(db_, error_out, "bundle parent insert failed"); return false;
        }
        bundle_id = sqlite3_column_int64(parent.value, 0);
    }
    std::int64_t revision_id = 0;
    {
        Statement revision(db_, "INSERT INTO au_predicate_bundle_revision_v2(predicate_bundle_id,revision_number,revision_state,created_at_utc) VALUES(?,COALESCE((SELECT MAX(revision_number)+1 FROM au_predicate_bundle_revision_v2 WHERE predicate_bundle_id=?),1),'DRAFT',?) RETURNING predicate_bundle_revision_id,revision_number");
        if (!revision) { rollback(); Error(db_, error_out, "prepare failed"); return false; }
        sqlite3_bind_int64(revision.value, 1, bundle_id);
        sqlite3_bind_int64(revision.value, 2, bundle_id);
        sqlite3_bind_int64(revision.value, 3, Millis(command.created_at_utc));
        if (sqlite3_step(revision.value) != SQLITE_ROW) {
            rollback(); Error(db_, error_out, "bundle revision insert failed"); return false;
        }
        revision_id = sqlite3_column_int64(revision.value, 0);
    }
    for(const auto& parameter:command.bundle.parameters){Statement s(db_,"INSERT INTO au_predicate_bundle_parameter_v2(predicate_bundle_revision_id,parameter_ordinal,parameter_name,value_builtin_type,value_schema_canonical_id,value_schema_revision,value_schema_sha256) VALUES(?,?,?,?,?,?,?)");sqlite3_bind_int64(s.value,1,revision_id);sqlite3_bind_int64(s.value,2,parameter.ordinal);BindText(s.value,3,parameter.name);BindType(s.value,4,parameter.value_type);if(sqlite3_step(s.value)!=SQLITE_DONE){rollback();Error(db_,error_out,"bundle parameter insert failed");return false;}}
    for(const auto& observation:command.bundle.observations){Statement s(db_,"INSERT INTO au_predicate_observation_v2(predicate_bundle_revision_id,observation_ordinal,stable_key,semantic_hook_id,source_kind,source_canonical_id,source_revision,source_sha256,pinned_guest_address,source_field,value_builtin_type,value_schema_canonical_id,value_schema_revision,value_schema_sha256) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");sqlite3_bind_int64(s.value,1,revision_id);sqlite3_bind_int64(s.value,2,observation.ordinal);BindText(s.value,3,observation.stable_key);BindText(s.value,4,observation.semantic_hook_id);BindText(s.value,5,ObservationKind(observation.source_kind));BindText(s.value,6,observation.source.canonical_id);sqlite3_bind_int64(s.value,7,observation.source.version);BindText(s.value,8,observation.source.signature_hash.ToHex());if(observation.pinned_guest_address&&*observation.pinned_guest_address<=static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))sqlite3_bind_int64(s.value,9,static_cast<std::int64_t>(*observation.pinned_guest_address));else sqlite3_bind_null(s.value,9);BindNullableText(s.value,10,observation.source_field);BindType(s.value,11,observation.value_type);if(sqlite3_step(s.value)!=SQLITE_DONE){rollback();Error(db_,error_out,"bundle observation insert failed");return false;}}
    for(const auto& baseline:command.bundle.baselines){Statement s(db_,"INSERT INTO au_predicate_baseline_v2 VALUES(?,?,?,?,?,?)");sqlite3_bind_int64(s.value,1,revision_id);sqlite3_bind_int64(s.value,2,baseline.ordinal);BindText(s.value,3,baseline.name);BindText(s.value,4,baseline.capture_hook_id);sqlite3_bind_int64(s.value,5,baseline.observation_ordinal);BindText(s.value,6,BaselinePolicy(baseline.update_policy));if(sqlite3_step(s.value)!=SQLITE_DONE){rollback();Error(db_,error_out,"bundle baseline insert failed");return false;}}
    for(const auto& check:command.bundle.checks){Statement s(db_,"INSERT INTO au_predicate_check_use_v2(predicate_bundle_revision_id,check_ordinal,stable_key,predicate_definition_revision_id,semantic_hook_id,occurrence_policy,occurrence_ordinal,guard_predicate_definition_revision_id,reaction,participates_in_aggregation,emit_evidence) VALUES(?,?,?,?,?,?,?,?,?,?,?)");sqlite3_bind_int64(s.value,1,revision_id);sqlite3_bind_int64(s.value,2,check.ordinal);BindText(s.value,3,check.use.canonical_id);sqlite3_bind_int64(s.value,4,check.predicate_definition_revision_id);BindText(s.value,5,check.use.semantic_point_id);BindText(s.value,6,Occurrence(check.occurrence));if(check.occurrence_ordinal)sqlite3_bind_int64(s.value,7,*check.occurrence_ordinal);else sqlite3_bind_null(s.value,7);if(check.guard_predicate_definition_revision_id)sqlite3_bind_int64(s.value,8,*check.guard_predicate_definition_revision_id);else sqlite3_bind_null(s.value,8);BindText(s.value,9,check.use.reaction==PredicateReaction::AbortOnFail?"ABORT_ON_FAIL":"RECORD_AND_CONTINUE");sqlite3_bind_int(s.value,10,check.use.participates_in_aggregation);sqlite3_bind_int(s.value,11,check.use.emit_evidence);if(sqlite3_step(s.value)!=SQLITE_DONE){rollback();Error(db_,error_out,"bundle check insert failed");return false;}
        for(const auto& witness:check.witnesses){Statement w(db_,"INSERT INTO au_predicate_witness_binding_v2(predicate_bundle_revision_id,check_ordinal,witness_ordinal,source_kind,source_ordinal,source_field,literal_kind,literal_integer,literal_real,literal_text,literal_blob) VALUES(?,?,?,?,?,?,?,?,?,?,?)");sqlite3_bind_int64(w.value,1,revision_id);sqlite3_bind_int64(w.value,2,check.ordinal);sqlite3_bind_int64(w.value,3,witness.witness_ordinal);BindText(w.value,4,WitnessSource(witness.source_kind));if(witness.source_ordinal)sqlite3_bind_int64(w.value,5,*witness.source_ordinal);else sqlite3_bind_null(w.value,5);BindNullableText(w.value,6,witness.source_field);if(witness.literal&&!BindLiteral(w.value,7,*witness.literal)){rollback();if(error_out)*error_out="unsupported witness literal";return false;}if(sqlite3_step(w.value)!=SQLITE_DONE){rollback();Error(db_,error_out,"witness binding insert failed");return false;}}
    }
    if(!Exec(db_,"COMMIT",error_out)){rollback();return false;}if(revision_id_out)*revision_id_out=revision_id;return true;
}

std::optional<PredicateBundleRevisionV2Snapshot>
SqliteAuthoringDb::GetPredicateBundleRevisionV2(std::int64_t revision_id) const {
    if (revision_id == 1) {
        PredicateBundleRevisionV2Snapshot empty{};
        empty.predicate_bundle_id = 1;
        empty.stable_key = "savor.predicate_bundle.empty";
        empty.name = "Empty predicate bundle";
        empty.description = "Canonical bundle containing no checks";
        empty.revision_state = "PUBLISHED";
        empty.bundle = EmptyPredicateBundleV1();
        return empty;
    }
    if(!db_||revision_id<=0)return std::nullopt;Statement root(db_,"SELECT b.predicate_bundle_id,b.stable_key,b.name,COALESCE(b.description,''),r.revision_number,r.revision_state,COALESCE(r.content_sha256,'') FROM au_predicate_bundle_revision_v2 r JOIN au_predicate_bundle_v2 b ON b.predicate_bundle_id=r.predicate_bundle_id WHERE r.predicate_bundle_revision_id=?");sqlite3_bind_int64(root.value,1,revision_id);if(sqlite3_step(root.value)!=SQLITE_ROW)return std::nullopt;PredicateBundleRevisionV2Snapshot out{};out.predicate_bundle_id=sqlite3_column_int64(root.value,0);out.stable_key=Text(root.value,1);out.name=Text(root.value,2);out.description=Text(root.value,3);out.revision_state=Text(root.value,5);out.bundle.bundle_revision_id=revision_id;out.bundle.canonical_id=out.stable_key;out.bundle.revision=sqlite3_column_int(root.value,4);out.bundle.content_sha256=Text(root.value,6);
    Statement parameters(db_,"SELECT parameter_ordinal,parameter_name,value_builtin_type,value_schema_canonical_id,value_schema_revision,value_schema_sha256 FROM au_predicate_bundle_parameter_v2 WHERE predicate_bundle_revision_id=? ORDER BY parameter_ordinal");sqlite3_bind_int64(parameters.value,1,revision_id);while(sqlite3_step(parameters.value)==SQLITE_ROW){auto type=ReadType(parameters.value,2);if(!type)return std::nullopt;out.bundle.parameters.push_back({static_cast<std::uint32_t>(sqlite3_column_int(parameters.value,0)),Text(parameters.value,1),*type});}
    Statement observations(db_,"SELECT observation_ordinal,stable_key,semantic_hook_id,source_kind,source_canonical_id,source_revision,source_sha256,pinned_guest_address,COALESCE(source_field,''),value_builtin_type,value_schema_canonical_id,value_schema_revision,value_schema_sha256 FROM au_predicate_observation_v2 WHERE predicate_bundle_revision_id=? ORDER BY observation_ordinal");sqlite3_bind_int64(observations.value,1,revision_id);while(sqlite3_step(observations.value)==SQLITE_ROW){auto kind=ParseObservationKind(Text(observations.value,3));auto hash=ContentHash256::FromHex(Text(observations.value,6));auto type=ReadType(observations.value,9);if(!kind||!hash||!type)return std::nullopt;PredicateObservationV1 observation{static_cast<std::uint32_t>(sqlite3_column_int(observations.value,0)),Text(observations.value,1),Text(observations.value,2),*kind,{Text(observations.value,4),static_cast<std::uint32_t>(sqlite3_column_int(observations.value,5)),*hash},*type};if(sqlite3_column_type(observations.value,7)!=SQLITE_NULL)observation.pinned_guest_address=static_cast<std::uint64_t>(sqlite3_column_int64(observations.value,7));observation.source_field=Text(observations.value,8);out.bundle.observations.push_back(std::move(observation));}
    Statement baselines(db_,"SELECT baseline_ordinal,baseline_name,capture_hook_id,observation_ordinal,update_policy FROM au_predicate_baseline_v2 WHERE predicate_bundle_revision_id=? ORDER BY baseline_ordinal");sqlite3_bind_int64(baselines.value,1,revision_id);while(sqlite3_step(baselines.value)==SQLITE_ROW)out.bundle.baselines.push_back({static_cast<std::uint32_t>(sqlite3_column_int(baselines.value,0)),Text(baselines.value,1),Text(baselines.value,2),static_cast<std::uint32_t>(sqlite3_column_int(baselines.value,3)),Text(baselines.value,4)=="LATEST"?PredicateBaselineUpdatePolicyV1::Latest:PredicateBaselineUpdatePolicyV1::First});
    Statement checks(db_,"SELECT check_ordinal,stable_key,predicate_definition_revision_id,semantic_hook_id,occurrence_policy,occurrence_ordinal,guard_predicate_definition_revision_id,reaction,participates_in_aggregation,emit_evidence FROM au_predicate_check_use_v2 WHERE predicate_bundle_revision_id=? ORDER BY check_ordinal");sqlite3_bind_int64(checks.value,1,revision_id);std::set<std::int64_t> definition_ids;while(sqlite3_step(checks.value)==SQLITE_ROW){PredicateCheckV1 check{};check.ordinal=sqlite3_column_int(checks.value,0);check.use.canonical_id=Text(checks.value,1);check.predicate_definition_revision_id=sqlite3_column_int64(checks.value,2);definition_ids.insert(check.predicate_definition_revision_id);check.use.semantic_point_id=Text(checks.value,3);auto occurrence=ParseOccurrence(Text(checks.value,4));if(!occurrence)return std::nullopt;check.occurrence=*occurrence;if(sqlite3_column_type(checks.value,5)!=SQLITE_NULL)check.occurrence_ordinal=sqlite3_column_int(checks.value,5);if(sqlite3_column_type(checks.value,6)!=SQLITE_NULL){check.guard_predicate_definition_revision_id=sqlite3_column_int64(checks.value,6);definition_ids.insert(*check.guard_predicate_definition_revision_id);}check.use.reaction=Text(checks.value,7)=="ABORT_ON_FAIL"?PredicateReaction::AbortOnFail:PredicateReaction::RecordAndContinue;check.use.participates_in_aggregation=sqlite3_column_int(checks.value,8)!=0;check.use.emit_evidence=sqlite3_column_int(checks.value,9)!=0;out.bundle.checks.push_back(std::move(check));}
    for(const auto id:definition_ids){auto definition=GetPredicateDefinitionRevisionV2(id);if(!definition||definition->revision_state!="PUBLISHED")return std::nullopt;out.bundle.definitions.push_back({id,std::move(definition->definition)});}
    for(auto& check:out.bundle.checks){const auto found=std::ranges::find(out.bundle.definitions,check.predicate_definition_revision_id,&ResolvedPredicateDefinitionV1::revision_id);if(found==out.bundle.definitions.end())return std::nullopt;Statement bindings(db_,"SELECT witness_ordinal,source_kind,source_ordinal,COALESCE(source_field,''),literal_kind,literal_integer,literal_real,literal_text,literal_blob FROM au_predicate_witness_binding_v2 WHERE predicate_bundle_revision_id=? AND check_ordinal=? ORDER BY witness_ordinal");sqlite3_bind_int64(bindings.value,1,revision_id);sqlite3_bind_int64(bindings.value,2,check.ordinal);while(sqlite3_step(bindings.value)==SQLITE_ROW){const auto ordinal=static_cast<std::uint32_t>(sqlite3_column_int(bindings.value,0));if(ordinal>=found->definition.witnesses.size())return std::nullopt;auto source=ParseWitnessSource(Text(bindings.value,1));if(!source)return std::nullopt;PredicateWitnessBindingV1 binding{.witness_ordinal=ordinal,.source_kind=*source,.source_field=Text(bindings.value,3),.value_type=found->definition.witnesses[ordinal].value_type};if(sqlite3_column_type(bindings.value,2)!=SQLITE_NULL)binding.source_ordinal=sqlite3_column_int(bindings.value,2);if(sqlite3_column_type(bindings.value,4)!=SQLITE_NULL){binding.literal=ReadLiteral(bindings.value,4,binding.value_type);if(!binding.literal)return std::nullopt;}check.witnesses.push_back(std::move(binding));}}
    return out;
}

bool SqliteAuthoringDb::PublishPredicateBundleRevisionV2(
    std::int64_t revision_id, types::UtcTimePoint published_at_utc,
    std::string* error_out) {
    auto snapshot=GetPredicateBundleRevisionV2(revision_id);if(!snapshot||snapshot->revision_state!="DRAFT"){if(error_out)*error_out="predicate bundle revision is not a draft";return false;}snapshot->bundle.content_sha256=ComputeResolvedPredicateBundleHashV1(snapshot->bundle);if(!ValidateBundleForPublication(snapshot->bundle,error_out))return false;Statement update(db_,"UPDATE au_predicate_bundle_revision_v2 SET revision_state='PUBLISHED',content_sha256=?,published_at_utc=? WHERE predicate_bundle_revision_id=? AND revision_state='DRAFT'");BindText(update.value,1,snapshot->bundle.content_sha256);sqlite3_bind_int64(update.value,2,Millis(published_at_utc));sqlite3_bind_int64(update.value,3,revision_id);if(sqlite3_step(update.value)!=SQLITE_DONE||sqlite3_changes(db_)!=1){Error(db_,error_out,"predicate bundle publication failed");return false;}return true;
}

} // namespace savor::db
