#include "SqliteAuthoringDb.h"

#include "../../SavorCore/Utils/Hash.h"

#include <algorithm>
#include <limits>
#include <map>
#include <ranges>

namespace savor::db {
namespace {

using namespace savor::runtime::program;
using namespace savor::runtime::program::composition;
using namespace savor::runtime::predicates;

struct Statement {
    sqlite3_stmt* value{};
    int prepare_result = SQLITE_MISUSE;
    Statement(sqlite3* db, const char* sql) { if (db) prepare_result = sqlite3_prepare_v2(db, sql, -1, &value, nullptr); }
    ~Statement() { if (value) sqlite3_finalize(value); }
    explicit operator bool() const noexcept { return value != nullptr; }
};
void SetError(sqlite3* db, std::string* out, std::string fallback) {
    if (!out) return;
    if (!db || sqlite3_extended_errcode(db) == SQLITE_OK) { *out = std::move(fallback); return; }
    *out = std::move(fallback) + ": " + sqlite3_errmsg(db) +
           " (sqlite=" + std::to_string(sqlite3_errcode(db)) +
           ", extended=" + std::to_string(sqlite3_extended_errcode(db)) + ")";
}
bool Exec(sqlite3* db, const char* sql, std::string* out) {
    char* message = nullptr; const auto rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc == SQLITE_OK) return true;
    if (out) *out = message ? message : sqlite3_errmsg(db); sqlite3_free(message); return false;
}
void BindText(sqlite3_stmt* statement, int index, std::string_view value) { sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT); }
void BindNullableText(sqlite3_stmt* statement, int index, const std::string& value) { if (value.empty()) sqlite3_bind_null(statement, index); else BindText(statement, index, value); }
std::string Text(sqlite3_stmt* statement, int column) { const auto* value = sqlite3_column_text(statement, column); return value ? reinterpret_cast<const char*>(value) : std::string{}; }
std::int64_t Millis(types::UtcTimePoint value) { return value.time_since_epoch().count(); }
bool IsRequestKey(std::string_view value) { return value.size() == 32 && std::ranges::all_of(value, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }); }
std::string RequestHash(std::initializer_list<std::string_view> parts) {
    std::string body; for (const auto part : parts) { body += std::to_string(part.size()); body.push_back(':'); body.append(part); body.push_back(';'); }
    return hash::sha256(body.data(), body.size());
}
std::optional<std::string> GenerateStableKey(sqlite3* db, std::string_view prefix) {
    Statement statement(db, "SELECT lower(hex(randomblob(16)))");
    if (!statement || sqlite3_step(statement.value) != SQLITE_ROW) return std::nullopt;
    return std::string(prefix) + Text(statement.value, 0);
}
bool LookupRequest(sqlite3* db, std::string_view operation, std::string_view key, std::string_view request_hash, std::int64_t* revision_id, std::string* error) {
    Statement statement(db, "SELECT request_sha256,revision_id FROM au_predicate_authoring_request WHERE operation_kind=? AND creation_request_key=?");
    if (!statement) {
        SetError(db, error, "predicate authoring request lookup preparation failed");
        if (revision_id) *revision_id = -1;
        return true;
    }
    BindText(statement.value, 1, operation); BindText(statement.value, 2, key);
    if (sqlite3_step(statement.value) != SQLITE_ROW) return false;
    if (Text(statement.value, 0) != request_hash) { if (error) *error = "predicate authoring request key was reused with different content"; if (revision_id) *revision_id = -1; return true; }
    if (revision_id) *revision_id = sqlite3_column_int64(statement.value, 1); return true;
}
bool InsertRequest(sqlite3* db, std::string_view operation, std::string_view key, std::string_view request_hash, std::int64_t parent_id, std::int64_t revision_id, types::UtcTimePoint when, std::string* error) {
    Statement statement(db, "INSERT INTO au_predicate_authoring_request(operation_kind,creation_request_key,request_sha256,parent_id,revision_id,created_at_utc) VALUES(?,?,?,?,?,?)");
    if (!statement) {
        SetError(db, error, "predicate authoring request ledger preparation failed");
        return false;
    }
    BindText(statement.value, 1, operation); BindText(statement.value, 2, key); BindText(statement.value, 3, request_hash);
    sqlite3_bind_int64(statement.value, 4, parent_id); sqlite3_bind_int64(statement.value, 5, revision_id); sqlite3_bind_int64(statement.value, 6, Millis(when));
    if (sqlite3_step(statement.value) == SQLITE_DONE) return true; SetError(db, error, "predicate authoring request ledger insert failed"); return false;
}

void BindType(sqlite3_stmt* statement, int first, const TypeRef& type) {
    if (type.is_named()) { sqlite3_bind_null(statement, first); BindText(statement, first + 1, type.named->canonical_id); sqlite3_bind_int64(statement, first + 2, type.named->version); BindText(statement, first + 3, type.named->schema_hash.ToHex()); }
    else { sqlite3_bind_int(statement, first, static_cast<int>(type.builtin)); sqlite3_bind_null(statement, first + 1); sqlite3_bind_null(statement, first + 2); sqlite3_bind_null(statement, first + 3); }
}
std::optional<TypeRef> ReadType(sqlite3_stmt* statement, int first) {
    if (sqlite3_column_type(statement, first) != SQLITE_NULL) return TypeRef::Builtin(static_cast<BuiltinType>(sqlite3_column_int(statement, first)));
    auto hash_value = ContentHash256::FromHex(Text(statement, first + 3)); if (!hash_value) return std::nullopt;
    return TypeRef::Named({Text(statement, first + 1), static_cast<std::uint32_t>(sqlite3_column_int64(statement, first + 2)), *hash_value});
}
bool BindLiteral(sqlite3_stmt* statement, int kind, const LiteralValue& value) {
    bool ok = true; const auto bind_integer = [&](std::string_view name, std::int64_t number) { BindText(statement, kind, name); sqlite3_bind_int64(statement, kind + 1, number); };
    std::visit([&](const auto& payload) { using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, UnitValue>) BindText(statement, kind, "UNIT");
        else if constexpr (std::is_same_v<T, bool>) bind_integer("BOOL", payload);
        else if constexpr (std::is_same_v<T, std::uint8_t>) bind_integer("U8", payload);
        else if constexpr (std::is_same_v<T, std::uint16_t>) bind_integer("U16", payload);
        else if constexpr (std::is_same_v<T, std::uint32_t>) bind_integer("U32", payload);
        else if constexpr (std::is_same_v<T, std::uint64_t>) { if (payload > static_cast<std::uint64_t>(INT64_MAX)) ok = false; else bind_integer("U64", static_cast<std::int64_t>(payload)); }
        else if constexpr (std::is_same_v<T, std::int32_t>) bind_integer("I32", payload);
        else if constexpr (std::is_same_v<T, std::int64_t>) bind_integer("I64", payload);
        else if constexpr (std::is_same_v<T, float>) { BindText(statement, kind, "F32"); sqlite3_bind_double(statement, kind + 2, payload); }
        else if constexpr (std::is_same_v<T, double>) { BindText(statement, kind, "F64"); sqlite3_bind_double(statement, kind + 2, payload); }
        else if constexpr (std::is_same_v<T, std::string>) { BindText(statement, kind, "TEXT"); BindText(statement, kind + 3, payload); }
        else if constexpr (std::is_same_v<T, std::vector<Byte>>) { BindText(statement, kind, "BYTES"); sqlite3_bind_blob(statement, kind + 4, payload.data(), static_cast<int>(payload.size()), SQLITE_TRANSIENT); }
        else if constexpr (std::is_same_v<T, EnumValue>) bind_integer("ENUM", payload.value); else ok = false;
    }, value.payload); return ok;
}
std::optional<LiteralValue> ReadLiteral(sqlite3_stmt* statement, int kind, TypeRef type) {
    const auto name = Text(statement, kind); const auto integer = sqlite3_column_int64(statement, kind + 1); LiteralValue value{.type = std::move(type)};
    if (name == "UNIT") value.payload = UnitValue{}; else if (name == "BOOL") value.payload = integer != 0;
    else if (name == "U8") value.payload = static_cast<std::uint8_t>(integer); else if (name == "U16") value.payload = static_cast<std::uint16_t>(integer);
    else if (name == "U32") value.payload = static_cast<std::uint32_t>(integer); else if (name == "U64") value.payload = static_cast<std::uint64_t>(integer);
    else if (name == "I32") value.payload = static_cast<std::int32_t>(integer); else if (name == "I64") value.payload = integer;
    else if (name == "F32") value.payload = static_cast<float>(sqlite3_column_double(statement, kind + 2)); else if (name == "F64") value.payload = sqlite3_column_double(statement, kind + 2);
    else if (name == "TEXT") value.payload = Text(statement, kind + 3); else if (name == "BYTES") { const auto* data = static_cast<const Byte*>(sqlite3_column_blob(statement, kind + 4)); const auto size = sqlite3_column_bytes(statement, kind + 4); value.payload = std::vector<Byte>(data, data + std::max(0, size)); }
    else if (name == "ENUM" && value.type.is_named()) value.payload = EnumValue{*value.type.named, integer}; else return std::nullopt; return value;
}

std::string SourceKind(PredicateWitnessSourceKindV1 value) { switch (value) { case PredicateWitnessSourceKindV1::ConcreteValue:return "CONCRETE_VALUE"; case PredicateWitnessSourceKindV1::DerivedStateQuery:return "DERIVED_STATE_QUERY"; case PredicateWitnessSourceKindV1::CurrentHookReceipt:return "CURRENT_HOOK_RECEIPT"; case PredicateWitnessSourceKindV1::PinnedGuestMemory:return "PINNED_GUEST_MEMORY"; case PredicateWitnessSourceKindV1::BaselineObservation:return "BASELINE_OBSERVATION"; } return {}; }
std::optional<PredicateWitnessSourceKindV1> ParseSourceKind(std::string_view value) { if(value=="CONCRETE_VALUE")return PredicateWitnessSourceKindV1::ConcreteValue;if(value=="DERIVED_STATE_QUERY")return PredicateWitnessSourceKindV1::DerivedStateQuery;if(value=="CURRENT_HOOK_RECEIPT")return PredicateWitnessSourceKindV1::CurrentHookReceipt;if(value=="PINNED_GUEST_MEMORY")return PredicateWitnessSourceKindV1::PinnedGuestMemory;if(value=="BASELINE_OBSERVATION")return PredicateWitnessSourceKindV1::BaselineObservation;return std::nullopt; }
std::string ObservationKind(PredicateObservationSourceKindV1 value) { switch(value){case PredicateObservationSourceKindV1::GuestAddress:return"GUEST_ADDRESS";case PredicateObservationSourceKindV1::RegisteredQuery:return"REGISTERED_QUERY";case PredicateObservationSourceKindV1::HookReceipt:return"HOOK_RECEIPT";case PredicateObservationSourceKindV1::RegisteredReducer:return"REGISTERED_REDUCER";}return{}; }
std::optional<PredicateObservationSourceKindV1> ParseObservationKind(std::string_view value){if(value=="GUEST_ADDRESS")return PredicateObservationSourceKindV1::GuestAddress;if(value=="REGISTERED_QUERY")return PredicateObservationSourceKindV1::RegisteredQuery;if(value=="HOOK_RECEIPT")return PredicateObservationSourceKindV1::HookReceipt;if(value=="REGISTERED_REDUCER")return PredicateObservationSourceKindV1::RegisteredReducer;return std::nullopt;}
std::string Occurrence(PredicateOccurrencePolicyV1 value){switch(value){case PredicateOccurrencePolicyV1::First:return"FIRST";case PredicateOccurrencePolicyV1::Every:return"EVERY";case PredicateOccurrencePolicyV1::Ordinal:return"ORDINAL";case PredicateOccurrencePolicyV1::GuardOnce:return"GUARD_ONCE";}return{};}
std::optional<PredicateOccurrencePolicyV1> ParseOccurrence(std::string_view value){if(value=="FIRST")return PredicateOccurrencePolicyV1::First;if(value=="EVERY")return PredicateOccurrencePolicyV1::Every;if(value=="ORDINAL")return PredicateOccurrencePolicyV1::Ordinal;if(value=="GUARD_ONCE")return PredicateOccurrencePolicyV1::GuardOnce;return std::nullopt;}
bool PublishedBinding(sqlite3* db, std::int64_t id){Statement s(db,"SELECT 1 FROM au_predicate_execution_binding_revision WHERE predicate_execution_binding_revision_id=? AND revision_state='PUBLISHED'");sqlite3_bind_int64(s.value,1,id);return sqlite3_step(s.value)==SQLITE_ROW;}
bool ReceiptField(std::string_view field,const TypeRef&type){return(field=="pc"&&type==TypeRef::Builtin(BuiltinType::U32))||((field=="movie_input_count"||field=="workset_epoch"||field=="vi_count")&&type==TypeRef::Builtin(BuiltinType::U64));}
bool ResolveBindingSources(
    const PredicateDefinition& definition,
    const std::vector<PredicateWitnessSourceBindingV1>& requested,
    std::vector<PredicateWitnessSourceBindingV1>& resolved,
    std::string* error)
{
    const auto catalog = BattlePredicateAuthoringCatalogV2();
    std::map<std::uint32_t, const PredicateWitnessSourceBindingV1*> supplied;
    for (const auto& source : requested)
    {
        if (source.witness_ordinal >= definition.witnesses.size() ||
            !supplied.emplace(source.witness_ordinal, &source).second)
        {
            if (error) *error = "predicate execution binding has a duplicate or unknown input";
            return false;
        }
    }
    resolved.clear();
    resolved.reserve(definition.witnesses.size());
    for (std::size_t ordinal = 0; ordinal < definition.witnesses.size(); ++ordinal)
    {
        const auto automatic = PlanPredicateSemanticWitnessSourceV1(
            definition.witnesses[ordinal], static_cast<std::uint32_t>(ordinal),
            catalog);
        const auto provided = supplied.find(static_cast<std::uint32_t>(ordinal));
        if (automatic)
        {
            if (provided != supplied.end() && *provided->second != *automatic)
            {
                if (error) *error =
                    "predicate semantic Battle data is captured automatically and cannot be overridden";
                return false;
            }
            resolved.push_back(*automatic);
            continue;
        }
        if (provided == supplied.end())
        {
            if (error) *error = "predicate execution binding is missing a concrete input";
            return false;
        }
        resolved.push_back(*provided->second);
    }
    return true;
}
bool ValidateBinding(PredicateExecutionBindingV1& binding,std::string* error){
    if(binding.execution_binding_revision_id<=0||binding.canonical_id.empty()||binding.revision==0||binding.definition.revision_id<=0||binding.definition.content_sha256!=ComputePredicateDefinitionHashV1(binding.definition.definition)||binding.witnesses.size()!=binding.definition.definition.witnesses.size()){if(error)*error="predicate execution binding identity or definition is invalid";return false;}
    const auto catalog=BattlePredicateAuthoringCatalogV2();
    for(std::size_t i=0;i<binding.witnesses.size();++i){const auto&s=binding.witnesses[i];if(s.witness_ordinal!=i||s.value_type!=binding.definition.definition.witnesses[i].value_type){if(error)*error="predicate witness source order or type is invalid";return false;}if(s.source_kind==PredicateWitnessSourceKindV1::ConcreteValue){if(!s.concrete_value||s.concrete_value->type!=s.value_type){if(error)*error="concrete predicate witness value is incomplete";return false;}}else if(s.concrete_value){if(error)*error="non-concrete predicate witness carries a concrete value";return false;}if(s.source_kind==PredicateWitnessSourceKindV1::CurrentHookReceipt&&!ReceiptField(s.source_field,s.value_type)){if(error)*error="predicate hook-receipt source is invalid";return false;}if(s.source_kind==PredicateWitnessSourceKindV1::DerivedStateQuery){const auto found=s.source?std::ranges::find(catalog.query_sources,*s.source,&PredicateAuthoringSourceV1::identity):catalog.query_sources.end();if(found==catalog.query_sources.end()||found->result_type!=s.value_type){if(error)*error="predicate derived-state query is unavailable or has the wrong type";return false;}}if(s.source_kind==PredicateWitnessSourceKindV1::BaselineObservation&&s.baseline_capture_hook_id.empty()){if(error)*error="predicate baseline capture hook is missing";return false;}}
    binding.content_sha256=ComputePredicateExecutionBindingHashV1(binding);return true;
}
bool WriteBindingSources(sqlite3*db,std::int64_t revision_id,const std::vector<PredicateWitnessSourceBindingV1>&sources,std::string*error){
    for(const auto&s:sources){Statement row(db,"INSERT INTO au_predicate_execution_binding_witness_source(predicate_execution_binding_revision_id,witness_ordinal,source_kind,value_builtin_type,value_schema_canonical_id,value_schema_revision,value_schema_sha256,literal_kind,literal_integer,literal_real,literal_text,literal_blob,observation_source_kind,source_canonical_id,source_revision,source_sha256,source_field,pinned_guest_address,baseline_capture_hook_id,baseline_update_policy) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");sqlite3_bind_int64(row.value,1,revision_id);sqlite3_bind_int64(row.value,2,s.witness_ordinal);BindText(row.value,3,SourceKind(s.source_kind));BindType(row.value,4,s.value_type);if(s.concrete_value&&!BindLiteral(row.value,8,*s.concrete_value)){if(error)*error="predicate concrete value cannot be persisted";return false;}BindText(row.value,13,ObservationKind(s.observation_source_kind));if(s.source){BindText(row.value,14,s.source->canonical_id);sqlite3_bind_int64(row.value,15,s.source->version);BindText(row.value,16,s.source->signature_hash.ToHex());}BindNullableText(row.value,17,s.source_field);if(s.pinned_guest_address&&*s.pinned_guest_address<=static_cast<std::uint64_t>(INT64_MAX))sqlite3_bind_int64(row.value,18,static_cast<std::int64_t>(*s.pinned_guest_address));BindNullableText(row.value,19,s.baseline_capture_hook_id);BindText(row.value,20,s.baseline_update_policy==PredicateBaselineUpdatePolicyV1::First?"FIRST":"LATEST");if(sqlite3_step(row.value)!=SQLITE_DONE){SetError(db,error,"binding witness-source insert failed");return false;}}
    return true;
}
bool ValidateGroupReferences(sqlite3*db,const std::vector<PredicateGroupMemberV1>&members,std::string*error){for(const auto&m:members){if(!PublishedBinding(db,m.execution_binding_revision_id)||(m.guard_execution_binding_revision_id&&!PublishedBinding(db,*m.guard_execution_binding_revision_id))){if(error)*error="predicate groups may reference only published execution bindings";return false;}}return true;}
bool WriteGroupMembers(sqlite3*db,std::int64_t revision_id,const std::vector<PredicateGroupMemberV1>&members,std::string*error){for(const auto&m:members){Statement member(db,"INSERT INTO au_predicate_group_member VALUES(?,?,?,?,?,?,?,?,?)");sqlite3_bind_int64(member.value,1,revision_id);sqlite3_bind_int64(member.value,2,m.ordinal);sqlite3_bind_int64(member.value,3,m.execution_binding_revision_id);BindText(member.value,4,Occurrence(m.occurrence));if(m.occurrence_ordinal)sqlite3_bind_int64(member.value,5,*m.occurrence_ordinal);if(m.guard_execution_binding_revision_id)sqlite3_bind_int64(member.value,6,*m.guard_execution_binding_revision_id);BindText(member.value,7,m.reaction==PredicateReaction::AbortOnFail?"ABORT_ON_FAIL":"RECORD_AND_CONTINUE");sqlite3_bind_int(member.value,8,m.participates_in_aggregation);sqlite3_bind_int(member.value,9,m.emit_evidence);if(sqlite3_step(member.value)!=SQLITE_DONE){SetError(db,error,"predicate group member insert failed");return false;}for(const auto&h:m.semantic_hook_ids){Statement hook(db,"INSERT INTO au_predicate_group_member_hook VALUES(?,?,?)");sqlite3_bind_int64(hook.value,1,revision_id);sqlite3_bind_int64(hook.value,2,m.ordinal);BindText(hook.value,3,h);if(sqlite3_step(hook.value)!=SQLITE_DONE){SetError(db,error,"predicate group hook insert failed");return false;}}}return true;}
PredicateAuthoringRevisionReceipt BindingReceipt(sqlite3*db,std::int64_t id,bool created=false,bool changed=false){PredicateAuthoringRevisionReceipt out{};Statement s(db,"SELECT b.predicate_execution_binding_id,b.stable_key,r.predicate_execution_binding_revision_id,r.revision_number,r.revision_state,r.semantic_sha256 FROM au_predicate_execution_binding_revision r JOIN au_predicate_execution_binding b ON b.predicate_execution_binding_id=r.predicate_execution_binding_id WHERE r.predicate_execution_binding_revision_id=?");sqlite3_bind_int64(s.value,1,id);if(sqlite3_step(s.value)==SQLITE_ROW){out.parent_id=sqlite3_column_int64(s.value,0);out.stable_key=Text(s.value,1);out.revision_id=sqlite3_column_int64(s.value,2);out.revision_number=sqlite3_column_int(s.value,3);out.revision_state=Text(s.value,4);out.semantic_sha256=Text(s.value,5);out.identity_created=created;out.semantic_changed=changed;}return out;}
PredicateAuthoringRevisionReceipt GroupReceipt(sqlite3*db,std::int64_t id,bool created=false,bool changed=false){PredicateAuthoringRevisionReceipt out{};Statement s(db,"SELECT g.predicate_group_id,g.stable_key,r.predicate_group_revision_id,r.revision_number,r.revision_state,r.semantic_sha256 FROM au_predicate_group_revision r JOIN au_predicate_group g ON g.predicate_group_id=r.predicate_group_id WHERE r.predicate_group_revision_id=?");sqlite3_bind_int64(s.value,1,id);if(sqlite3_step(s.value)==SQLITE_ROW){out.parent_id=sqlite3_column_int64(s.value,0);out.stable_key=Text(s.value,1);out.revision_id=sqlite3_column_int64(s.value,2);out.revision_number=sqlite3_column_int(s.value,3);out.revision_state=Text(s.value,4);out.semantic_sha256=Text(s.value,5);out.identity_created=created;out.semantic_changed=changed;}return out;}
template<class Summary>void BindListQuery(sqlite3_stmt*s,const PredicateRevisionListQueryV2&q,int limit){if(q.revision_state)BindText(s,1,*q.revision_state);else sqlite3_bind_null(s,1);BindText(s,2,q.search_text);if(q.before_revision_id)sqlite3_bind_int64(s,3,*q.before_revision_id);else sqlite3_bind_null(s,3);sqlite3_bind_int(s,4,limit+1);}

} // namespace

bool SqliteAuthoringDb::CreatePredicateExecutionBindingDraft(const CreatePredicateExecutionBindingDraftCommand&command,PredicateAuthoringRevisionReceipt*out,std::string*error){
    if(!db_||!IsRequestKey(command.creation_request_key)||command.name.empty()){if(error)*error="predicate execution-binding creation request is invalid";return false;}
    const auto definition=GetPredicateDefinitionRevisionV2(command.body.predicate_definition_revision_id);if(!definition||definition->revision_state!="PUBLISHED"||definition->content_sha256.empty()){if(error)*error="predicate execution binding requires one exact published definition";return false;}
    std::vector<PredicateWitnessSourceBindingV1> resolved_sources;
    if(!ResolveBindingSources(definition->definition,command.body.witnesses,resolved_sources,error))return false;
    PredicateExecutionBindingV1 binding{.execution_binding_revision_id=1,.canonical_id="semantic",.revision=1,.definition={definition->predicate_definition_revision_id,definition->content_sha256,definition->definition},.witnesses=std::move(resolved_sources)};if(!ValidateBinding(binding,error))return false;
    const auto semantic=ComputePredicateExecutionBindingSemanticHashV1(binding);const auto request_hash=RequestHash({command.name,command.description,semantic});
    if(!Exec(db_,"BEGIN IMMEDIATE",error))return false;const auto rollback=[&]{(void)sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);};std::int64_t replay=0;
    if(LookupRequest(db_,"CREATE_BINDING",command.creation_request_key,request_hash,&replay,error)){if(replay<0){rollback();return false;}const auto receipt=BindingReceipt(db_,replay);if(!Exec(db_,"COMMIT",error)){rollback();return false;}if(out)*out=receipt;return receipt.revision_id>0;}
    const auto stable_key=GenerateStableKey(db_,"predicate.binding/");if(!stable_key){rollback();SetError(db_,error,"binding stable-key generation failed");return false;}
    Statement parent(db_,"INSERT INTO au_predicate_execution_binding(stable_key,name,description,created_at_utc,updated_at_utc) VALUES(?,?,?,?,?)");BindText(parent.value,1,*stable_key);BindText(parent.value,2,command.name);BindText(parent.value,3,command.description);sqlite3_bind_int64(parent.value,4,Millis(command.created_at_utc));sqlite3_bind_int64(parent.value,5,Millis(command.created_at_utc));if(sqlite3_step(parent.value)!=SQLITE_DONE){rollback();SetError(db_,error,"binding parent insert failed");return false;}const auto parent_id=sqlite3_last_insert_rowid(db_);
    Statement revision(db_,"INSERT INTO au_predicate_execution_binding_revision(predicate_execution_binding_id,revision_number,revision_state,predicate_definition_revision_id,predicate_definition_sha256,semantic_sha256,created_at_utc) VALUES(?,1,'DRAFT',?,?,?,?)");sqlite3_bind_int64(revision.value,1,parent_id);sqlite3_bind_int64(revision.value,2,definition->predicate_definition_revision_id);BindText(revision.value,3,definition->content_sha256);BindText(revision.value,4,semantic);sqlite3_bind_int64(revision.value,5,Millis(command.created_at_utc));if(sqlite3_step(revision.value)!=SQLITE_DONE){rollback();SetError(db_,error,"binding revision insert failed");return false;}const auto revision_id=sqlite3_last_insert_rowid(db_);
    if(!WriteBindingSources(db_,revision_id,binding.witnesses,error)||!InsertRequest(db_,"CREATE_BINDING",command.creation_request_key,request_hash,parent_id,revision_id,command.created_at_utc,error)||!Exec(db_,"COMMIT",error)){rollback();return false;}if(out)*out=BindingReceipt(db_,revision_id,true,true);return true;
}

bool SqliteAuthoringDb::SavePredicateExecutionBindingDraft(const SavePredicateExecutionBindingDraftCommand&command,PredicateAuthoringRevisionReceipt*out,std::string*error){
    if(!db_||command.predicate_execution_binding_id<=0){if(error)*error="predicate execution-binding identity is invalid";return false;}
    const auto definition=GetPredicateDefinitionRevisionV2(command.body.predicate_definition_revision_id);if(!definition||definition->revision_state!="PUBLISHED"||definition->content_sha256.empty()){if(error)*error="predicate execution binding requires one exact published definition";return false;}
    std::string stable_key;
    {
        Statement parent(db_,"SELECT stable_key FROM au_predicate_execution_binding WHERE predicate_execution_binding_id=?");
        if(!parent){SetError(db_,error,"binding parent lookup preparation failed");return false;}
        sqlite3_bind_int64(parent.value,1,command.predicate_execution_binding_id);
        if(sqlite3_step(parent.value)!=SQLITE_ROW){if(error)*error="predicate execution-binding identity was not found";return false;}
        stable_key=Text(parent.value,0);
    }
    std::vector<PredicateWitnessSourceBindingV1> resolved_sources;
    if(!ResolveBindingSources(definition->definition,command.body.witnesses,resolved_sources,error))return false;
    PredicateExecutionBindingV1 binding{.execution_binding_revision_id=1,.canonical_id=stable_key,.revision=1,.definition={definition->predicate_definition_revision_id,definition->content_sha256,definition->definition},.witnesses=std::move(resolved_sources)};if(!ValidateBinding(binding,error))return false;const auto semantic=ComputePredicateExecutionBindingSemanticHashV1(binding);
    if(!Exec(db_,"BEGIN IMMEDIATE",error))return false;const auto rollback=[&]{(void)sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);};
    Statement same(db_,"SELECT predicate_execution_binding_revision_id FROM au_predicate_execution_binding_revision WHERE predicate_execution_binding_id=? AND semantic_sha256=?");sqlite3_bind_int64(same.value,1,command.predicate_execution_binding_id);BindText(same.value,2,semantic);if(sqlite3_step(same.value)==SQLITE_ROW){const auto id=sqlite3_column_int64(same.value,0);if(!Exec(db_,"COMMIT",error)){rollback();return false;}if(out)*out=BindingReceipt(db_,id);return true;}
    std::int64_t revision_id=0;Statement draft(db_,"SELECT predicate_execution_binding_revision_id FROM au_predicate_execution_binding_revision WHERE predicate_execution_binding_id=? AND revision_state='DRAFT'");sqlite3_bind_int64(draft.value,1,command.predicate_execution_binding_id);
    if(sqlite3_step(draft.value)==SQLITE_ROW){revision_id=sqlite3_column_int64(draft.value,0);Statement clear(db_,"DELETE FROM au_predicate_execution_binding_witness_source WHERE predicate_execution_binding_revision_id=?");sqlite3_bind_int64(clear.value,1,revision_id);if(sqlite3_step(clear.value)!=SQLITE_DONE){rollback();return false;}Statement update(db_,"UPDATE au_predicate_execution_binding_revision SET predicate_definition_revision_id=?,predicate_definition_sha256=?,semantic_sha256=?,content_sha256=NULL WHERE predicate_execution_binding_revision_id=? AND revision_state='DRAFT'");sqlite3_bind_int64(update.value,1,definition->predicate_definition_revision_id);BindText(update.value,2,definition->content_sha256);BindText(update.value,3,semantic);sqlite3_bind_int64(update.value,4,revision_id);if(sqlite3_step(update.value)!=SQLITE_DONE){rollback();return false;}}
    else{Statement revision(db_,"INSERT INTO au_predicate_execution_binding_revision(predicate_execution_binding_id,revision_number,revision_state,predicate_definition_revision_id,predicate_definition_sha256,semantic_sha256,created_at_utc) VALUES(?,COALESCE((SELECT MAX(revision_number)+1 FROM au_predicate_execution_binding_revision WHERE predicate_execution_binding_id=?),1),'DRAFT',?,?,?,?)");sqlite3_bind_int64(revision.value,1,command.predicate_execution_binding_id);sqlite3_bind_int64(revision.value,2,command.predicate_execution_binding_id);sqlite3_bind_int64(revision.value,3,definition->predicate_definition_revision_id);BindText(revision.value,4,definition->content_sha256);BindText(revision.value,5,semantic);sqlite3_bind_int64(revision.value,6,Millis(command.saved_at_utc));if(sqlite3_step(revision.value)!=SQLITE_DONE){rollback();SetError(db_,error,"binding draft creation failed");return false;}revision_id=sqlite3_last_insert_rowid(db_);}
    if(!WriteBindingSources(db_,revision_id,binding.witnesses,error)||!Exec(db_,"COMMIT",error)){rollback();return false;}if(out)*out=BindingReceipt(db_,revision_id,false,true);return true;
}

bool SqliteAuthoringDb::DuplicatePredicateExecutionBinding(const DuplicatePredicateExecutionBindingCommand&command,PredicateAuthoringRevisionReceipt*out,std::string*error){
    if(!IsRequestKey(command.creation_request_key)||command.name.empty()){if(error)*error="predicate execution-binding duplicate request is invalid";return false;}const auto source=GetPredicateExecutionBindingRevision(command.source_revision_id);if(!source){if(error)*error="source predicate execution-binding revision was not found";return false;}
    const auto source_id=std::to_string(command.source_revision_id);const auto request_hash=RequestHash({source_id,command.name,command.description,source->semantic_sha256});if(!Exec(db_,"BEGIN IMMEDIATE",error))return false;const auto rollback=[&]{(void)sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);};std::int64_t replay=0;
    if(LookupRequest(db_,"DUPLICATE_BINDING",command.creation_request_key,request_hash,&replay,error)){if(replay<0){rollback();return false;}const auto receipt=BindingReceipt(db_,replay);if(!Exec(db_,"COMMIT",error)){rollback();return false;}if(out)*out=receipt;return true;}
    const auto stable_key=GenerateStableKey(db_,"predicate.binding/");if(!stable_key){rollback();return false;}Statement parent(db_,"INSERT INTO au_predicate_execution_binding(stable_key,name,description,created_at_utc,updated_at_utc) VALUES(?,?,?,?,?)");BindText(parent.value,1,*stable_key);BindText(parent.value,2,command.name);BindText(parent.value,3,command.description);sqlite3_bind_int64(parent.value,4,Millis(command.created_at_utc));sqlite3_bind_int64(parent.value,5,Millis(command.created_at_utc));if(sqlite3_step(parent.value)!=SQLITE_DONE){rollback();return false;}const auto parent_id=sqlite3_last_insert_rowid(db_);
    Statement revision(db_,"INSERT INTO au_predicate_execution_binding_revision(predicate_execution_binding_id,revision_number,revision_state,predicate_definition_revision_id,predicate_definition_sha256,semantic_sha256,created_at_utc) VALUES(?,1,'DRAFT',?,?,?,?)");sqlite3_bind_int64(revision.value,1,parent_id);sqlite3_bind_int64(revision.value,2,source->binding.definition.revision_id);BindText(revision.value,3,source->binding.definition.content_sha256);BindText(revision.value,4,source->semantic_sha256);sqlite3_bind_int64(revision.value,5,Millis(command.created_at_utc));if(sqlite3_step(revision.value)!=SQLITE_DONE){rollback();return false;}const auto revision_id=sqlite3_last_insert_rowid(db_);
    if(!WriteBindingSources(db_,revision_id,source->binding.witnesses,error)||!InsertRequest(db_,"DUPLICATE_BINDING",command.creation_request_key,request_hash,parent_id,revision_id,command.created_at_utc,error)||!Exec(db_,"COMMIT",error)){rollback();return false;}if(out)*out=BindingReceipt(db_,revision_id,true,true);return true;
}

std::optional<PredicateExecutionBindingRevisionSnapshot>SqliteAuthoringDb::GetPredicateExecutionBindingRevision(std::int64_t id)const{
    Statement root(db_,"SELECT b.predicate_execution_binding_id,b.stable_key,b.name,COALESCE(b.description,''),r.revision_number,r.revision_state,r.semantic_sha256,COALESCE(r.content_sha256,''),r.predicate_definition_revision_id,r.predicate_definition_sha256 FROM au_predicate_execution_binding_revision r JOIN au_predicate_execution_binding b ON b.predicate_execution_binding_id=r.predicate_execution_binding_id WHERE r.predicate_execution_binding_revision_id=?");sqlite3_bind_int64(root.value,1,id);if(sqlite3_step(root.value)!=SQLITE_ROW)return std::nullopt;const auto definition_id=sqlite3_column_int64(root.value,8);const auto definition=GetPredicateDefinitionRevisionV2(definition_id);if(!definition)return std::nullopt;
    PredicateExecutionBindingRevisionSnapshot out{};out.predicate_execution_binding_id=sqlite3_column_int64(root.value,0);out.stable_key=Text(root.value,1);out.name=Text(root.value,2);out.description=Text(root.value,3);out.revision_state=Text(root.value,5);out.semantic_sha256=Text(root.value,6);out.binding.execution_binding_revision_id=id;out.binding.canonical_id=out.stable_key;out.binding.revision=sqlite3_column_int(root.value,4);out.binding.content_sha256=Text(root.value,7);out.binding.definition={definition_id,Text(root.value,9),definition->definition};
    Statement sources(db_,"SELECT witness_ordinal,source_kind,value_builtin_type,value_schema_canonical_id,value_schema_revision,value_schema_sha256,literal_kind,literal_integer,literal_real,literal_text,literal_blob,observation_source_kind,source_canonical_id,source_revision,source_sha256,COALESCE(source_field,''),pinned_guest_address,COALESCE(baseline_capture_hook_id,''),COALESCE(baseline_update_policy,'FIRST') FROM au_predicate_execution_binding_witness_source WHERE predicate_execution_binding_revision_id=? ORDER BY witness_ordinal");sqlite3_bind_int64(sources.value,1,id);while(sqlite3_step(sources.value)==SQLITE_ROW){auto kind=ParseSourceKind(Text(sources.value,1));auto type=ReadType(sources.value,2);auto observation=ParseObservationKind(Text(sources.value,11));if(!kind||!type||!observation)return std::nullopt;PredicateWitnessSourceBindingV1 source{.witness_ordinal=static_cast<std::uint32_t>(sqlite3_column_int(sources.value,0)),.source_kind=*kind,.value_type=*type,.observation_source_kind=*observation,.source_field=Text(sources.value,15),.baseline_capture_hook_id=Text(sources.value,17),.baseline_update_policy=Text(sources.value,18)=="LATEST"?PredicateBaselineUpdatePolicyV1::Latest:PredicateBaselineUpdatePolicyV1::First};if(sqlite3_column_type(sources.value,6)!=SQLITE_NULL){source.concrete_value=ReadLiteral(sources.value,6,*type);if(!source.concrete_value)return std::nullopt;}if(sqlite3_column_type(sources.value,12)!=SQLITE_NULL){auto h=ContentHash256::FromHex(Text(sources.value,14));if(!h)return std::nullopt;source.source=ExactDependencyIdentity{Text(sources.value,12),static_cast<std::uint32_t>(sqlite3_column_int64(sources.value,13)),*h};}if(sqlite3_column_type(sources.value,16)!=SQLITE_NULL)source.pinned_guest_address=static_cast<std::uint64_t>(sqlite3_column_int64(sources.value,16));out.binding.witnesses.push_back(std::move(source));}return out;
}

PredicateRevisionPageV2<PredicateExecutionBindingRevisionSummary>SqliteAuthoringDb::ListPredicateExecutionBindingRevisions(const PredicateRevisionListQueryV2&q)const{
    PredicateRevisionPageV2<PredicateExecutionBindingRevisionSummary>out;if(!db_)return out;const auto limit=std::clamp(q.limit,1,500);Statement s(db_,"SELECT r.predicate_execution_binding_revision_id,r.revision_number,b.stable_key,b.name,COALESCE(b.description,''),r.revision_state,r.semantic_sha256,COALESCE(r.content_sha256,''),r.predicate_definition_revision_id,(SELECT COUNT(1) FROM au_predicate_execution_binding_witness_source w WHERE w.predicate_execution_binding_revision_id=r.predicate_execution_binding_revision_id) FROM au_predicate_execution_binding_revision r JOIN au_predicate_execution_binding b ON b.predicate_execution_binding_id=r.predicate_execution_binding_id WHERE (?1 IS NULL OR r.revision_state=?1) AND (?2='' OR b.stable_key LIKE '%'||?2||'%' OR b.name LIKE '%'||?2||'%' OR b.description LIKE '%'||?2||'%') AND (?3 IS NULL OR r.predicate_execution_binding_revision_id<?3) ORDER BY r.predicate_execution_binding_revision_id DESC LIMIT ?4");BindListQuery<PredicateExecutionBindingRevisionSummary>(s.value,q,limit);while(sqlite3_step(s.value)==SQLITE_ROW)out.items.push_back({sqlite3_column_int64(s.value,0),sqlite3_column_int(s.value,1),Text(s.value,2),Text(s.value,3),Text(s.value,4),Text(s.value,5),Text(s.value,6),Text(s.value,7),sqlite3_column_int64(s.value,8),sqlite3_column_int(s.value,9)});if(out.items.size()>static_cast<std::size_t>(limit)){out.items.resize(limit);out.next_before_revision_id=out.items.back().predicate_execution_binding_revision_id;}return out;
}

bool SqliteAuthoringDb::PublishPredicateExecutionBindingRevision(std::int64_t id,types::UtcTimePoint when,PredicateAuthoringRevisionReceipt*out,std::string*error){auto snapshot=GetPredicateExecutionBindingRevision(id);if(!snapshot){if(error)*error="predicate execution-binding revision was not found";return false;}if(snapshot->revision_state=="PUBLISHED"){if(out)*out=BindingReceipt(db_,id);return true;}if(snapshot->revision_state!="DRAFT"||!ValidateBinding(snapshot->binding,error))return false;Statement update(db_,"UPDATE au_predicate_execution_binding_revision SET revision_state='PUBLISHED',content_sha256=?,published_at_utc=? WHERE predicate_execution_binding_revision_id=? AND revision_state='DRAFT'");BindText(update.value,1,snapshot->binding.content_sha256);sqlite3_bind_int64(update.value,2,Millis(when));sqlite3_bind_int64(update.value,3,id);if(sqlite3_step(update.value)!=SQLITE_DONE||sqlite3_changes(db_)!=1){SetError(db_,error,"predicate execution binding publication failed");return false;}if(out)*out=BindingReceipt(db_,id);return true;}
bool SqliteAuthoringDb::AbandonPredicateExecutionBindingDraft(std::int64_t id,std::string*error){Statement s(db_,"DELETE FROM au_predicate_execution_binding_revision WHERE predicate_execution_binding_revision_id=? AND revision_state='DRAFT'");sqlite3_bind_int64(s.value,1,id);if(sqlite3_step(s.value)!=SQLITE_DONE||sqlite3_changes(db_)!=1){if(error)*error="predicate execution binding revision is not an abandonable draft";return false;}return true;}

bool SqliteAuthoringDb::CreatePredicateGroupDraft(const CreatePredicateGroupDraftCommand&command,PredicateAuthoringRevisionReceipt*out,std::string*error){
    if(!db_||!IsRequestKey(command.creation_request_key)||command.name.empty()||!ValidateGroupReferences(db_,command.body.members,error)){if(error&&error->empty())*error="predicate group creation request is invalid";return false;}
    ResolvedPredicateGroupV1 group{.predicate_group_revision_id=1,.canonical_id="semantic",.revision=1,.members=command.body.members};const auto semantic=ComputeResolvedPredicateGroupSemanticHashV1(group);const auto request_hash=RequestHash({command.name,command.description,semantic});
    if(!Exec(db_,"BEGIN IMMEDIATE",error))return false;const auto rollback=[&]{(void)sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);};std::int64_t replay=0;
    if(LookupRequest(db_,"CREATE_GROUP",command.creation_request_key,request_hash,&replay,error)){if(replay<0){rollback();return false;}const auto receipt=GroupReceipt(db_,replay);if(!Exec(db_,"COMMIT",error)){rollback();return false;}if(out)*out=receipt;return receipt.revision_id>0;}
    const auto stable_key=GenerateStableKey(db_,"predicate.group/");if(!stable_key){rollback();return false;}Statement parent(db_,"INSERT INTO au_predicate_group(stable_key,name,description,created_at_utc,updated_at_utc) VALUES(?,?,?,?,?)");BindText(parent.value,1,*stable_key);BindText(parent.value,2,command.name);BindText(parent.value,3,command.description);sqlite3_bind_int64(parent.value,4,Millis(command.created_at_utc));sqlite3_bind_int64(parent.value,5,Millis(command.created_at_utc));if(sqlite3_step(parent.value)!=SQLITE_DONE){rollback();SetError(db_,error,"group parent insert failed");return false;}const auto parent_id=sqlite3_last_insert_rowid(db_);
    Statement revision(db_,"INSERT INTO au_predicate_group_revision(predicate_group_id,revision_number,revision_state,semantic_sha256,created_at_utc) VALUES(?,1,'DRAFT',?,?)");sqlite3_bind_int64(revision.value,1,parent_id);BindText(revision.value,2,semantic);sqlite3_bind_int64(revision.value,3,Millis(command.created_at_utc));if(sqlite3_step(revision.value)!=SQLITE_DONE){rollback();SetError(db_,error,"group revision insert failed");return false;}const auto revision_id=sqlite3_last_insert_rowid(db_);
    if(!WriteGroupMembers(db_,revision_id,command.body.members,error)||!InsertRequest(db_,"CREATE_GROUP",command.creation_request_key,request_hash,parent_id,revision_id,command.created_at_utc,error)||!Exec(db_,"COMMIT",error)){rollback();return false;}if(out)*out=GroupReceipt(db_,revision_id,true,true);return true;
}

bool SqliteAuthoringDb::SavePredicateGroupDraft(const SavePredicateGroupDraftCommand&command,PredicateAuthoringRevisionReceipt*out,std::string*error){
    if(!db_||command.predicate_group_id<=0||!ValidateGroupReferences(db_,command.body.members,error))return false;
    std::string stable_key;
    {
        Statement parent(db_,"SELECT stable_key FROM au_predicate_group WHERE predicate_group_id=?");
        if(!parent){SetError(db_,error,"group parent lookup preparation failed");return false;}
        sqlite3_bind_int64(parent.value,1,command.predicate_group_id);
        if(sqlite3_step(parent.value)!=SQLITE_ROW){if(error)*error="predicate group identity was not found";return false;}
        stable_key=Text(parent.value,0);
    }
    ResolvedPredicateGroupV1 group{.predicate_group_revision_id=1,.canonical_id=stable_key,.revision=1,.members=command.body.members};const auto semantic=ComputeResolvedPredicateGroupSemanticHashV1(group);if(!Exec(db_,"BEGIN IMMEDIATE",error))return false;const auto rollback=[&]{(void)sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);};
    Statement same(db_,"SELECT predicate_group_revision_id FROM au_predicate_group_revision WHERE predicate_group_id=? AND semantic_sha256=?");sqlite3_bind_int64(same.value,1,command.predicate_group_id);BindText(same.value,2,semantic);if(sqlite3_step(same.value)==SQLITE_ROW){const auto id=sqlite3_column_int64(same.value,0);if(!Exec(db_,"COMMIT",error)){rollback();return false;}if(out)*out=GroupReceipt(db_,id);return true;}
    std::int64_t revision_id=0;Statement draft(db_,"SELECT predicate_group_revision_id FROM au_predicate_group_revision WHERE predicate_group_id=? AND revision_state='DRAFT'");sqlite3_bind_int64(draft.value,1,command.predicate_group_id);
    if(sqlite3_step(draft.value)==SQLITE_ROW){revision_id=sqlite3_column_int64(draft.value,0);Statement hooks(db_,"DELETE FROM au_predicate_group_member_hook WHERE predicate_group_revision_id=?");sqlite3_bind_int64(hooks.value,1,revision_id);if(sqlite3_step(hooks.value)!=SQLITE_DONE){rollback();return false;}Statement members(db_,"DELETE FROM au_predicate_group_member WHERE predicate_group_revision_id=?");sqlite3_bind_int64(members.value,1,revision_id);if(sqlite3_step(members.value)!=SQLITE_DONE){rollback();return false;}Statement update(db_,"UPDATE au_predicate_group_revision SET semantic_sha256=?,content_sha256=NULL WHERE predicate_group_revision_id=? AND revision_state='DRAFT'");BindText(update.value,1,semantic);sqlite3_bind_int64(update.value,2,revision_id);if(sqlite3_step(update.value)!=SQLITE_DONE){rollback();return false;}}
    else{Statement revision(db_,"INSERT INTO au_predicate_group_revision(predicate_group_id,revision_number,revision_state,semantic_sha256,created_at_utc) VALUES(?,COALESCE((SELECT MAX(revision_number)+1 FROM au_predicate_group_revision WHERE predicate_group_id=?),1),'DRAFT',?,?)");sqlite3_bind_int64(revision.value,1,command.predicate_group_id);sqlite3_bind_int64(revision.value,2,command.predicate_group_id);BindText(revision.value,3,semantic);sqlite3_bind_int64(revision.value,4,Millis(command.saved_at_utc));if(sqlite3_step(revision.value)!=SQLITE_DONE){rollback();SetError(db_,error,"group draft creation failed");return false;}revision_id=sqlite3_last_insert_rowid(db_);}
    if(!WriteGroupMembers(db_,revision_id,command.body.members,error)||!Exec(db_,"COMMIT",error)){rollback();return false;}if(out)*out=GroupReceipt(db_,revision_id,false,true);return true;
}

bool SqliteAuthoringDb::DuplicatePredicateGroup(const DuplicatePredicateGroupCommand&command,PredicateAuthoringRevisionReceipt*out,std::string*error){
    if(!IsRequestKey(command.creation_request_key)||command.name.empty()){if(error)*error="predicate group duplicate request is invalid";return false;}const auto source=GetPredicateGroupRevision(command.source_revision_id);if(!source){if(error)*error="source predicate group revision was not found";return false;}const auto source_id=std::to_string(command.source_revision_id);const auto request_hash=RequestHash({source_id,command.name,command.description,source->semantic_sha256});
    if(!Exec(db_,"BEGIN IMMEDIATE",error))return false;const auto rollback=[&]{(void)sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);};std::int64_t replay=0;if(LookupRequest(db_,"DUPLICATE_GROUP",command.creation_request_key,request_hash,&replay,error)){if(replay<0){rollback();return false;}const auto receipt=GroupReceipt(db_,replay);if(!Exec(db_,"COMMIT",error)){rollback();return false;}if(out)*out=receipt;return true;}
    const auto stable_key=GenerateStableKey(db_,"predicate.group/");if(!stable_key){rollback();return false;}Statement parent(db_,"INSERT INTO au_predicate_group(stable_key,name,description,created_at_utc,updated_at_utc) VALUES(?,?,?,?,?)");BindText(parent.value,1,*stable_key);BindText(parent.value,2,command.name);BindText(parent.value,3,command.description);sqlite3_bind_int64(parent.value,4,Millis(command.created_at_utc));sqlite3_bind_int64(parent.value,5,Millis(command.created_at_utc));if(sqlite3_step(parent.value)!=SQLITE_DONE){rollback();return false;}const auto parent_id=sqlite3_last_insert_rowid(db_);Statement revision(db_,"INSERT INTO au_predicate_group_revision(predicate_group_id,revision_number,revision_state,semantic_sha256,created_at_utc) VALUES(?,1,'DRAFT',?,?)");sqlite3_bind_int64(revision.value,1,parent_id);BindText(revision.value,2,source->semantic_sha256);sqlite3_bind_int64(revision.value,3,Millis(command.created_at_utc));if(sqlite3_step(revision.value)!=SQLITE_DONE){rollback();return false;}const auto revision_id=sqlite3_last_insert_rowid(db_);
    if(!WriteGroupMembers(db_,revision_id,source->group.members,error)||!InsertRequest(db_,"DUPLICATE_GROUP",command.creation_request_key,request_hash,parent_id,revision_id,command.created_at_utc,error)||!Exec(db_,"COMMIT",error)){rollback();return false;}if(out)*out=GroupReceipt(db_,revision_id,true,true);return true;
}

std::optional<PredicateGroupRevisionSnapshot>SqliteAuthoringDb::GetPredicateGroupRevision(std::int64_t id)const{
    Statement root(db_,"SELECT g.predicate_group_id,g.stable_key,g.name,COALESCE(g.description,''),r.revision_number,r.revision_state,r.semantic_sha256,COALESCE(r.content_sha256,'') FROM au_predicate_group_revision r JOIN au_predicate_group g ON g.predicate_group_id=r.predicate_group_id WHERE r.predicate_group_revision_id=?");sqlite3_bind_int64(root.value,1,id);if(sqlite3_step(root.value)!=SQLITE_ROW)return std::nullopt;PredicateGroupRevisionSnapshot out{};out.predicate_group_id=sqlite3_column_int64(root.value,0);out.stable_key=Text(root.value,1);out.name=Text(root.value,2);out.description=Text(root.value,3);out.revision_state=Text(root.value,5);out.semantic_sha256=Text(root.value,6);out.group={.predicate_group_revision_id=id,.canonical_id=out.stable_key,.revision=static_cast<std::uint32_t>(sqlite3_column_int(root.value,4)),.content_sha256=Text(root.value,7)};
    Statement members(db_,"SELECT member_ordinal,predicate_execution_binding_revision_id,occurrence_policy,occurrence_ordinal,guard_execution_binding_revision_id,reaction,participates_in_aggregation,emit_evidence FROM au_predicate_group_member WHERE predicate_group_revision_id=? ORDER BY member_ordinal");sqlite3_bind_int64(members.value,1,id);while(sqlite3_step(members.value)==SQLITE_ROW){auto occurrence=ParseOccurrence(Text(members.value,2));if(!occurrence)return std::nullopt;PredicateGroupMemberV1 m{.ordinal=static_cast<std::uint32_t>(sqlite3_column_int(members.value,0)),.execution_binding_revision_id=sqlite3_column_int64(members.value,1),.occurrence=*occurrence,.reaction=Text(members.value,5)=="ABORT_ON_FAIL"?PredicateReaction::AbortOnFail:PredicateReaction::RecordAndContinue,.participates_in_aggregation=sqlite3_column_int(members.value,6)!=0,.emit_evidence=sqlite3_column_int(members.value,7)!=0};if(sqlite3_column_type(members.value,3)!=SQLITE_NULL)m.occurrence_ordinal=sqlite3_column_int(members.value,3);if(sqlite3_column_type(members.value,4)!=SQLITE_NULL)m.guard_execution_binding_revision_id=sqlite3_column_int64(members.value,4);Statement hooks(db_,"SELECT semantic_hook_id FROM au_predicate_group_member_hook WHERE predicate_group_revision_id=? AND member_ordinal=? ORDER BY semantic_hook_id");sqlite3_bind_int64(hooks.value,1,id);sqlite3_bind_int64(hooks.value,2,m.ordinal);while(sqlite3_step(hooks.value)==SQLITE_ROW)m.semantic_hook_ids.push_back(Text(hooks.value,0));out.group.members.push_back(std::move(m));}return out;
}

PredicateRevisionPageV2<PredicateGroupRevisionSummary>SqliteAuthoringDb::ListPredicateGroupRevisions(const PredicateRevisionListQueryV2&q)const{
    PredicateRevisionPageV2<PredicateGroupRevisionSummary>out;if(!db_)return out;const auto limit=std::clamp(q.limit,1,500);Statement s(db_,"SELECT r.predicate_group_revision_id,r.revision_number,g.stable_key,g.name,COALESCE(g.description,''),r.revision_state,r.semantic_sha256,COALESCE(r.content_sha256,''),(SELECT COUNT(1) FROM au_predicate_group_member m WHERE m.predicate_group_revision_id=r.predicate_group_revision_id),(SELECT COUNT(1) FROM au_predicate_group_member_hook h WHERE h.predicate_group_revision_id=r.predicate_group_revision_id) FROM au_predicate_group_revision r JOIN au_predicate_group g ON g.predicate_group_id=r.predicate_group_id WHERE (?1 IS NULL OR r.revision_state=?1) AND (?2='' OR g.stable_key LIKE '%'||?2||'%' OR g.name LIKE '%'||?2||'%' OR g.description LIKE '%'||?2||'%') AND (?3 IS NULL OR r.predicate_group_revision_id<?3) ORDER BY r.predicate_group_revision_id DESC LIMIT ?4");BindListQuery<PredicateGroupRevisionSummary>(s.value,q,limit);while(sqlite3_step(s.value)==SQLITE_ROW)out.items.push_back({sqlite3_column_int64(s.value,0),sqlite3_column_int(s.value,1),Text(s.value,2),Text(s.value,3),Text(s.value,4),Text(s.value,5),Text(s.value,6),Text(s.value,7),sqlite3_column_int(s.value,8),sqlite3_column_int(s.value,9)});if(out.items.size()>static_cast<std::size_t>(limit)){out.items.resize(limit);out.next_before_revision_id=out.items.back().predicate_group_revision_id;}return out;
}

bool SqliteAuthoringDb::PublishPredicateGroupRevision(std::int64_t id,types::UtcTimePoint when,PredicateAuthoringRevisionReceipt*out,std::string*error){
    auto snapshot=GetPredicateGroupRevision(id);if(!snapshot){if(error)*error="predicate group revision was not found";return false;}if(snapshot->revision_state=="PUBLISHED"){if(out)*out=GroupReceipt(db_,id);return true;}if(snapshot->revision_state!="DRAFT"){if(error)*error="predicate group revision is not publishable";return false;}
    std::vector<PredicateExecutionBindingV1>bindings;for(const auto&m:snapshot->group.members){auto binding=GetPredicateExecutionBindingRevision(m.execution_binding_revision_id);if(!binding||binding->revision_state!="PUBLISHED"){if(error)*error="predicate group references an unpublished execution binding";return false;}bindings.push_back(binding->binding);if(m.guard_execution_binding_revision_id&&std::ranges::find(bindings,*m.guard_execution_binding_revision_id,&PredicateExecutionBindingV1::execution_binding_revision_id)==bindings.end()){auto guard=GetPredicateExecutionBindingRevision(*m.guard_execution_binding_revision_id);if(!guard||guard->revision_state!="PUBLISHED"){if(error)*error="predicate group guard is unpublished";return false;}bindings.push_back(guard->binding);}}
    std::ranges::sort(bindings,{},&PredicateExecutionBindingV1::execution_binding_revision_id);bindings.erase(std::unique(bindings.begin(),bindings.end(),[](const auto&a,const auto&b){return a.execution_binding_revision_id==b.execution_binding_revision_id;}),bindings.end());snapshot->group.content_sha256=ComputeResolvedPredicateGroupHashV1(snapshot->group);PredicateExecutionPackageV1 package{.hook_contract=BattlePredicateHookContractV1(),.group=snapshot->group,.execution_bindings=std::move(bindings)};package.content_sha256=ComputePredicateExecutionPackageHashV1(package);const auto validation=ValidatePredicateExecutionPackageV1(package);if(!validation){if(error)*error=validation.code+": "+validation.message;return false;}Statement update(db_,"UPDATE au_predicate_group_revision SET revision_state='PUBLISHED',content_sha256=?,published_at_utc=? WHERE predicate_group_revision_id=? AND revision_state='DRAFT'");BindText(update.value,1,snapshot->group.content_sha256);sqlite3_bind_int64(update.value,2,Millis(when));sqlite3_bind_int64(update.value,3,id);if(sqlite3_step(update.value)!=SQLITE_DONE||sqlite3_changes(db_)!=1){SetError(db_,error,"predicate group publication failed");return false;}if(out)*out=GroupReceipt(db_,id);return true;
}
bool SqliteAuthoringDb::AbandonPredicateGroupDraft(std::int64_t id,std::string*error){Statement s(db_,"DELETE FROM au_predicate_group_revision WHERE predicate_group_revision_id=? AND revision_state='DRAFT'");sqlite3_bind_int64(s.value,1,id);if(sqlite3_step(s.value)!=SQLITE_DONE||sqlite3_changes(db_)!=1){if(error)*error="predicate group revision is not an abandonable draft";return false;}return true;}

} // namespace savor::db
