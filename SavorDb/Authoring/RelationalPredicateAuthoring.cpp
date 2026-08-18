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
    int prepare_result = SQLITE_MISUSE;
    Statement(sqlite3* db, const char* sql) {
        if (db) prepare_result = sqlite3_prepare_v2(db, sql, -1, &value, nullptr);
    }
    ~Statement() { if (value) sqlite3_finalize(value); }
    explicit operator bool() const noexcept { return value != nullptr; }
};

void Error(sqlite3* db, std::string* out, std::string fallback) {
    if (!out) return;
    if (!db || sqlite3_extended_errcode(db) == SQLITE_OK) { *out = std::move(fallback); return; }
    *out = std::move(fallback) + ": " + sqlite3_errmsg(db) +
           " (sqlite=" + std::to_string(sqlite3_errcode(db)) +
           ", extended=" + std::to_string(sqlite3_extended_errcode(db)) + ")";
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
    PredicateEvaluationPolicy policy{
        .predicate_group_revision_id = 1,
        .execution_binding_revision_id = 1,
        .semantic_point_id = "authoring.validation",
    };
    const auto lowered = LowerPredicate(definition, policy, module);
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

bool IsRequestKey(std::string_view value);
std::string RequestHash(std::initializer_list<std::string_view> parts);
std::optional<std::string> GenerateStableKey(sqlite3* db, std::string_view prefix);
PredicateDefinition BuildDefinition(std::string stable_key, std::uint32_t revision,
                                    const PredicateDefinitionDraftBody& body);
bool WriteDefinitionChildren(sqlite3* db, std::int64_t revision_id,
                             const PredicateDefinitionDraftBody& body,
                             std::string* error_out);
PredicateAuthoringRevisionReceipt DefinitionReceipt(
    sqlite3* db, std::int64_t revision_id,
    bool identity_created = false, bool semantic_changed = false);
bool LookupCreationRequest(sqlite3* db, std::string_view operation,
                           std::string_view request_key,
                           std::string_view request_hash,
                           std::int64_t* revision_id_out,
                           std::string* error_out);
bool InsertCreationRequest(sqlite3* db, std::string_view operation,
                           std::string_view request_key,
                           std::string_view request_hash,
                           std::int64_t parent_id, std::int64_t revision_id,
                           types::UtcTimePoint when, std::string* error_out);

bool SqliteAuthoringDb::CreatePredicateDefinitionDraft(
    const CreatePredicateDefinitionDraftCommand& command,
    PredicateAuthoringRevisionReceipt* receipt_out,
    std::string* error_out) {
    if (!db_ || !IsRequestKey(command.creation_request_key) || command.name.empty()) {
        if (error_out) *error_out = "predicate definition creation request is invalid";
        return false;
    }
    auto semantic_definition = BuildDefinition("semantic", 1, command.body);
    if (DefinitionHash(semantic_definition, error_out).empty()) return false;
    const auto semantic = ComputePredicateDefinitionSemanticHashV1(semantic_definition);
    const auto request_hash = RequestHash({command.name, command.description, semantic});
    if (!Exec(db_, "BEGIN IMMEDIATE", error_out)) return false;
    const auto rollback = [&]() { (void)sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); };
    std::int64_t replay_revision = 0;
    if (LookupCreationRequest(db_, "CREATE_DEFINITION", command.creation_request_key,
                              request_hash, &replay_revision, error_out)) {
        if (replay_revision < 0) { rollback(); return false; }
        const auto receipt = DefinitionReceipt(db_, replay_revision);
        if (!Exec(db_, "COMMIT", error_out)) { rollback(); return false; }
        if (receipt_out) *receipt_out = receipt;
        return receipt.revision_id > 0;
    }
    const auto stable_key = GenerateStableKey(db_, "predicate.definition/");
    if (!stable_key) { rollback(); Error(db_, error_out, "stable-key generation failed"); return false; }
    Statement parent(db_, "INSERT INTO au_predicate_definition_v2(stable_key,name,description,created_at_utc,updated_at_utc) VALUES(?,?,?,?,?)");
    if (!parent) { rollback(); Error(db_, error_out, "definition parent insert preparation failed"); return false; }
    BindText(parent.value, 1, *stable_key);
    BindText(parent.value, 2, command.name);
    BindText(parent.value, 3, command.description);
    sqlite3_bind_int64(parent.value, 4, Millis(command.created_at_utc));
    sqlite3_bind_int64(parent.value, 5, Millis(command.created_at_utc));
    if (sqlite3_step(parent.value) != SQLITE_DONE) { rollback(); Error(db_, error_out, "definition parent insert failed"); return false; }
    const auto parent_id = sqlite3_last_insert_rowid(db_);
    Statement revision(db_, "INSERT INTO au_predicate_definition_revision_v2(predicate_definition_id,revision_number,revision_state,root_node_ordinal,semantic_sha256,created_at_utc) VALUES(?,1,'DRAFT',?,?,?)");
    if (!revision) { rollback(); Error(db_, error_out, "definition revision insert preparation failed"); return false; }
    sqlite3_bind_int64(revision.value, 1, parent_id);
    sqlite3_bind_int64(revision.value, 2, static_cast<sqlite3_int64>(command.body.root_expression));
    BindText(revision.value, 3, semantic);
    sqlite3_bind_int64(revision.value, 4, Millis(command.created_at_utc));
    if (sqlite3_step(revision.value) != SQLITE_DONE) { rollback(); Error(db_, error_out, "definition revision insert failed"); return false; }
    const auto revision_id = sqlite3_last_insert_rowid(db_);
    if (!WriteDefinitionChildren(db_, revision_id, command.body, error_out) ||
        !InsertCreationRequest(db_, "CREATE_DEFINITION", command.creation_request_key,
                               request_hash, parent_id, revision_id,
                               command.created_at_utc, error_out) ||
        !Exec(db_, "COMMIT", error_out)) {
        rollback(); return false;
    }
    if (receipt_out) *receipt_out = DefinitionReceipt(db_, revision_id, true, true);
    return true;
}

bool SqliteAuthoringDb::SavePredicateDefinitionDraft(
    const SavePredicateDefinitionDraftCommand& command,
    PredicateAuthoringRevisionReceipt* receipt_out,
    std::string* error_out) {
    if (!db_ || command.predicate_definition_id <= 0) {
        if (error_out) *error_out = "predicate definition identity is invalid";
        return false;
    }
    if (!Exec(db_, "BEGIN IMMEDIATE", error_out)) return false;
    const auto rollback = [&]() { (void)sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); };
    Statement parent(db_, "SELECT stable_key FROM au_predicate_definition_v2 WHERE predicate_definition_id=?");
    sqlite3_bind_int64(parent.value, 1, command.predicate_definition_id);
    if (sqlite3_step(parent.value) != SQLITE_ROW) { rollback(); if (error_out) *error_out = "predicate definition identity was not found"; return false; }
    const auto stable_key = Text(parent.value, 0);
    auto semantic_definition = BuildDefinition(stable_key, 1, command.body);
    if (DefinitionHash(semantic_definition, error_out).empty()) { rollback(); return false; }
    const auto semantic = ComputePredicateDefinitionSemanticHashV1(semantic_definition);
    Statement same(db_, "SELECT predicate_definition_revision_id FROM au_predicate_definition_revision_v2 WHERE predicate_definition_id=? AND semantic_sha256=?");
    sqlite3_bind_int64(same.value, 1, command.predicate_definition_id);
    BindText(same.value, 2, semantic);
    if (sqlite3_step(same.value) == SQLITE_ROW) {
        const auto id = sqlite3_column_int64(same.value, 0);
        if (!Exec(db_, "COMMIT", error_out)) { rollback(); return false; }
        if (receipt_out) *receipt_out = DefinitionReceipt(db_, id);
        return true;
    }
    std::int64_t revision_id = 0;
    Statement draft(db_, "SELECT predicate_definition_revision_id FROM au_predicate_definition_revision_v2 WHERE predicate_definition_id=? AND revision_state='DRAFT'");
    sqlite3_bind_int64(draft.value, 1, command.predicate_definition_id);
    if (sqlite3_step(draft.value) == SQLITE_ROW) {
        revision_id = sqlite3_column_int64(draft.value, 0);
        for (const char* sql : {
                 "DELETE FROM au_predicate_expression_edge_v2 WHERE predicate_definition_revision_id=?",
                 "DELETE FROM au_predicate_expression_node_v2 WHERE predicate_definition_revision_id=?",
                 "DELETE FROM au_predicate_witness_v2 WHERE predicate_definition_revision_id=?"}) {
            Statement clear(db_, sql); sqlite3_bind_int64(clear.value, 1, revision_id);
            if (sqlite3_step(clear.value) != SQLITE_DONE) { rollback(); Error(db_, error_out, "definition draft reset failed"); return false; }
        }
        Statement update(db_, "UPDATE au_predicate_definition_revision_v2 SET root_node_ordinal=?,semantic_sha256=?,content_sha256=NULL WHERE predicate_definition_revision_id=? AND revision_state='DRAFT'");
        sqlite3_bind_int64(update.value, 1, static_cast<sqlite3_int64>(command.body.root_expression));
        BindText(update.value, 2, semantic);
        sqlite3_bind_int64(update.value, 3, revision_id);
        if (sqlite3_step(update.value) != SQLITE_DONE) { rollback(); Error(db_, error_out, "definition draft update failed"); return false; }
    } else {
        Statement revision(db_, "INSERT INTO au_predicate_definition_revision_v2(predicate_definition_id,revision_number,revision_state,root_node_ordinal,semantic_sha256,created_at_utc) VALUES(?,COALESCE((SELECT MAX(revision_number)+1 FROM au_predicate_definition_revision_v2 WHERE predicate_definition_id=?),1),'DRAFT',?,?,?)");
        sqlite3_bind_int64(revision.value, 1, command.predicate_definition_id);
        sqlite3_bind_int64(revision.value, 2, command.predicate_definition_id);
        sqlite3_bind_int64(revision.value, 3, static_cast<sqlite3_int64>(command.body.root_expression));
        BindText(revision.value, 4, semantic);
        sqlite3_bind_int64(revision.value, 5, Millis(command.saved_at_utc));
        if (sqlite3_step(revision.value) != SQLITE_DONE) { rollback(); Error(db_, error_out, "definition draft creation failed"); return false; }
        revision_id = sqlite3_last_insert_rowid(db_);
    }
    if (!WriteDefinitionChildren(db_, revision_id, command.body, error_out) ||
        !Exec(db_, "COMMIT", error_out)) { rollback(); return false; }
    if (receipt_out) *receipt_out = DefinitionReceipt(db_, revision_id, false, true);
    return true;
}

bool SqliteAuthoringDb::DuplicatePredicateDefinition(
    const DuplicatePredicateDefinitionCommand& command,
    PredicateAuthoringRevisionReceipt* receipt_out,
    std::string* error_out) {
    if (!db_ || !IsRequestKey(command.creation_request_key)
        || command.source_revision_id <= 0 || command.name.empty()) {
        if (error_out) *error_out = "predicate definition duplicate request is invalid";
        return false;
    }
    const auto source = GetPredicateDefinitionRevisionV2(command.source_revision_id);
    if (!source) {
        if (error_out) *error_out = "source predicate definition revision was not found";
        return false;
    }
    const PredicateDefinitionDraftBody body{
        source->definition.witnesses,
        source->definition.expression,
        source->definition.root_expression,
    };
    auto semantic_definition = BuildDefinition("semantic", 1, body);
    if (DefinitionHash(semantic_definition, error_out).empty()) return false;
    const auto semantic = ComputePredicateDefinitionSemanticHashV1(semantic_definition);
    const auto request_hash = RequestHash({
        std::to_string(command.source_revision_id), command.name,
        command.description, semantic});

    if (!Exec(db_, "BEGIN IMMEDIATE", error_out)) return false;
    const auto rollback = [&]() {
        (void)sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    };
    std::int64_t replay_revision = 0;
    if (LookupCreationRequest(db_, "DUPLICATE_DEFINITION",
                              command.creation_request_key, request_hash,
                              &replay_revision, error_out)) {
        if (replay_revision < 0) {
            rollback();
            return false;
        }
        const auto receipt = DefinitionReceipt(db_, replay_revision);
        if (!Exec(db_, "COMMIT", error_out)) {
            rollback();
            return false;
        }
        if (receipt_out) *receipt_out = receipt;
        return receipt.revision_id > 0;
    }

    const auto stable_key = GenerateStableKey(db_, "predicate.definition/");
    if (!stable_key) {
        rollback();
        Error(db_, error_out, "stable-key generation failed");
        return false;
    }
    Statement parent(db_,
        "INSERT INTO au_predicate_definition_v2(stable_key,name,description,created_at_utc,updated_at_utc) VALUES(?,?,?,?,?)");
    BindText(parent.value, 1, *stable_key);
    BindText(parent.value, 2, command.name);
    BindText(parent.value, 3, command.description);
    sqlite3_bind_int64(parent.value, 4, Millis(command.created_at_utc));
    sqlite3_bind_int64(parent.value, 5, Millis(command.created_at_utc));
    if (sqlite3_step(parent.value) != SQLITE_DONE) {
        rollback();
        Error(db_, error_out, "definition duplicate parent insert failed");
        return false;
    }
    const auto parent_id = sqlite3_last_insert_rowid(db_);
    Statement revision(db_,
        "INSERT INTO au_predicate_definition_revision_v2(predicate_definition_id,revision_number,revision_state,root_node_ordinal,semantic_sha256,created_at_utc) VALUES(?,1,'DRAFT',?,?,?)");
    sqlite3_bind_int64(revision.value, 1, parent_id);
    sqlite3_bind_int64(revision.value, 2,
                       static_cast<sqlite3_int64>(body.root_expression));
    BindText(revision.value, 3, semantic);
    sqlite3_bind_int64(revision.value, 4, Millis(command.created_at_utc));
    if (sqlite3_step(revision.value) != SQLITE_DONE) {
        rollback();
        Error(db_, error_out, "definition duplicate revision insert failed");
        return false;
    }
    const auto revision_id = sqlite3_last_insert_rowid(db_);
    if (!WriteDefinitionChildren(db_, revision_id, body, error_out)
        || !InsertCreationRequest(db_, "DUPLICATE_DEFINITION",
                                  command.creation_request_key, request_hash,
                                  parent_id, revision_id,
                                  command.created_at_utc, error_out)
        || !Exec(db_, "COMMIT", error_out)) {
        rollback();
        return false;
    }
    if (receipt_out)
        *receipt_out = DefinitionReceipt(db_, revision_id, true, true);
    return true;
}

std::optional<PredicateDefinitionRevisionV2Snapshot>
SqliteAuthoringDb::GetPredicateDefinitionRevisionV2(std::int64_t revision_id) const {
    if(!db_||revision_id<=0)return std::nullopt;
    Statement root(db_,"SELECT d.predicate_definition_id,d.stable_key,d.name,COALESCE(d.description,''),r.revision_number,r.revision_state,r.semantic_sha256,COALESCE(r.content_sha256,''),r.root_node_ordinal FROM au_predicate_definition_revision_v2 r JOIN au_predicate_definition_v2 d ON d.predicate_definition_id=r.predicate_definition_id WHERE r.predicate_definition_revision_id=?");sqlite3_bind_int64(root.value,1,revision_id);if(sqlite3_step(root.value)!=SQLITE_ROW)return std::nullopt;
    PredicateDefinitionRevisionV2Snapshot out{};out.predicate_definition_id=sqlite3_column_int64(root.value,0);out.predicate_definition_revision_id=revision_id;out.stable_key=Text(root.value,1);out.name=Text(root.value,2);out.description=Text(root.value,3);out.definition.canonical_id=out.stable_key;out.definition.revision=sqlite3_column_int(root.value,4);out.revision_state=Text(root.value,5);out.semantic_sha256=Text(root.value,6);out.content_sha256=Text(root.value,7);out.definition.root_expression=static_cast<std::size_t>(sqlite3_column_int64(root.value,8));out.definition.source_name="SavorDb.PredicateV2";
    Statement witnesses(db_,"SELECT witness_name,value_builtin_type,value_schema_canonical_id,value_schema_revision,value_schema_sha256 FROM au_predicate_witness_v2 WHERE predicate_definition_revision_id=? ORDER BY witness_ordinal");sqlite3_bind_int64(witnesses.value,1,revision_id);while(sqlite3_step(witnesses.value)==SQLITE_ROW){auto type=ReadType(witnesses.value,1);if(!type)return std::nullopt;out.definition.witnesses.push_back({Text(witnesses.value,0),*type});}
    Statement nodes(db_,"SELECT node_kind,result_builtin_type,result_schema_canonical_id,result_schema_revision,result_schema_sha256,witness_ordinal,literal_kind,literal_integer,literal_real,literal_text,literal_blob,reducer_canonical_id,reducer_revision,reducer_sha256,source_label,node_ordinal FROM au_predicate_expression_node_v2 WHERE predicate_definition_revision_id=? ORDER BY node_ordinal");sqlite3_bind_int64(nodes.value,1,revision_id);while(sqlite3_step(nodes.value)==SQLITE_ROW){PredicateExpressionNode node{};auto kind=ParseNodeKind(Text(nodes.value,0));auto type=ReadType(nodes.value,1);if(!kind||!type)return std::nullopt;node.kind=*kind;node.result_type=*type;if(sqlite3_column_type(nodes.value,5)!=SQLITE_NULL)node.witness_index=sqlite3_column_int64(nodes.value,5);if(sqlite3_column_type(nodes.value,6)!=SQLITE_NULL){node.literal=ReadLiteral(nodes.value,6,*type);if(!node.literal)return std::nullopt;}if(sqlite3_column_type(nodes.value,11)!=SQLITE_NULL){const auto hash=ContentHash256::FromHex(Text(nodes.value,13));if(!hash)return std::nullopt;node.reducer=ExactDependencyIdentity{Text(nodes.value,11),static_cast<std::uint32_t>(sqlite3_column_int64(nodes.value,12)),*hash};}node.source_label=Text(nodes.value,14);const auto ordinal=sqlite3_column_int64(nodes.value,15);Statement edges(db_,"SELECT operand_node_ordinal FROM au_predicate_expression_edge_v2 WHERE predicate_definition_revision_id=? AND node_ordinal=? ORDER BY operand_ordinal");sqlite3_bind_int64(edges.value,1,revision_id);sqlite3_bind_int64(edges.value,2,ordinal);while(sqlite3_step(edges.value)==SQLITE_ROW)node.operands.push_back(sqlite3_column_int64(edges.value,0));out.definition.expression.push_back(std::move(node));}
    return out;
}

PredicateRevisionPageV2<PredicateDefinitionRevisionV2Summary>
SqliteAuthoringDb::ListPredicateDefinitionRevisionsV2(
    const PredicateRevisionListQueryV2& query) const {
    PredicateRevisionPageV2<PredicateDefinitionRevisionV2Summary> out;
    if (!db_) return out;
    const auto limit = std::clamp(query.limit, 1, 500);
    Statement st(db_,
        "SELECT r.predicate_definition_revision_id,r.revision_number,d.stable_key,d.name,COALESCE(d.description,''),r.revision_state,r.semantic_sha256,COALESCE(r.content_sha256,''),"
        "(SELECT COUNT(1) FROM au_predicate_witness_v2 w WHERE w.predicate_definition_revision_id=r.predicate_definition_revision_id),"
        "(SELECT COUNT(1) FROM au_predicate_expression_node_v2 n WHERE n.predicate_definition_revision_id=r.predicate_definition_revision_id) "
        "FROM au_predicate_definition_revision_v2 r JOIN au_predicate_definition_v2 d ON d.predicate_definition_id=r.predicate_definition_id "
        "WHERE (?1 IS NULL OR r.revision_state=?1) AND (?2='' OR d.stable_key LIKE '%'||?2||'%' OR d.name LIKE '%'||?2||'%' OR COALESCE(d.description,'') LIKE '%'||?2||'%') "
        "AND (?3 IS NULL OR r.predicate_definition_revision_id<?3) ORDER BY r.predicate_definition_revision_id DESC LIMIT ?4");
    if (!st) return out;
    if (query.revision_state) BindText(st.value, 1, *query.revision_state); else sqlite3_bind_null(st.value, 1);
    BindText(st.value, 2, query.search_text);
    if (query.before_revision_id) sqlite3_bind_int64(st.value, 3, *query.before_revision_id); else sqlite3_bind_null(st.value, 3);
    sqlite3_bind_int(st.value, 4, limit + 1);
    while (sqlite3_step(st.value) == SQLITE_ROW) {
        PredicateDefinitionRevisionV2Summary item{};
        item.predicate_definition_revision_id = sqlite3_column_int64(st.value, 0);
        item.revision_number = sqlite3_column_int(st.value, 1);
        item.stable_key = Text(st.value, 2);
        item.name = Text(st.value, 3);
        item.description = Text(st.value, 4);
        item.revision_state = Text(st.value, 5);
        item.semantic_sha256 = Text(st.value, 6);
        item.content_sha256 = Text(st.value, 7);
        item.witness_count = sqlite3_column_int(st.value, 8);
        item.expression_node_count = sqlite3_column_int(st.value, 9);
        out.items.push_back(std::move(item));
    }
    if (out.items.size() > static_cast<std::size_t>(limit)) {
        out.items.resize(static_cast<std::size_t>(limit));
        out.next_before_revision_id = out.items.back().predicate_definition_revision_id;
    }
    return out;
}

bool SqliteAuthoringDb::PublishPredicateDefinitionRevisionV2(
    std::int64_t revision_id, types::UtcTimePoint published_at_utc,
    PredicateAuthoringRevisionReceipt* receipt_out,
    std::string* error_out) {
    const auto snapshot = GetPredicateDefinitionRevisionV2(revision_id);
    if (!snapshot) { if (error_out) *error_out = "predicate definition revision was not found"; return false; }
    if (snapshot->revision_state == "PUBLISHED") {
        if (receipt_out) *receipt_out = DefinitionReceipt(db_, revision_id);
        return true;
    }
    if (snapshot->revision_state != "DRAFT") { if (error_out) *error_out = "predicate definition revision is not publishable"; return false; }
    const auto content = DefinitionHash(snapshot->definition, error_out); if (content.empty()) return false;
    Statement update(db_,"UPDATE au_predicate_definition_revision_v2 SET revision_state='PUBLISHED',content_sha256=?,published_at_utc=? WHERE predicate_definition_revision_id=? AND revision_state='DRAFT'");BindText(update.value,1,content);sqlite3_bind_int64(update.value,2,Millis(published_at_utc));sqlite3_bind_int64(update.value,3,revision_id);if(sqlite3_step(update.value)!=SQLITE_DONE||sqlite3_changes(db_)!=1){Error(db_,error_out,"predicate definition publication failed");return false;}
    if (receipt_out) *receipt_out = DefinitionReceipt(db_, revision_id);
    return true;
}

bool IsRequestKey(std::string_view value) {
    return value.size() == 32 && std::ranges::all_of(value, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string RequestHash(std::initializer_list<std::string_view> parts) {
    std::string body;
    for (const auto part : parts) {
        body += std::to_string(part.size());
        body.push_back(':');
        body.append(part);
        body.push_back(';');
    }
    return hash::sha256(body.data(), body.size());
}

std::optional<std::string> GenerateStableKey(sqlite3* db, std::string_view prefix) {
    Statement statement(db, "SELECT lower(hex(randomblob(16)))");
    if (!statement || sqlite3_step(statement.value) != SQLITE_ROW) return std::nullopt;
    return std::string(prefix) + Text(statement.value, 0);
}

PredicateDefinition BuildDefinition(
    std::string stable_key,
    std::uint32_t revision,
    const PredicateDefinitionDraftBody& body) {
    return PredicateDefinition{
        .canonical_id = std::move(stable_key),
        .revision = revision,
        .source_name = "SavorDb.PredicateV2",
        .witnesses = body.witnesses,
        .expression = body.expression,
        .root_expression = body.root_expression,
    };
}

bool WriteDefinitionChildren(
    sqlite3* db,
    std::int64_t revision_id,
    const PredicateDefinitionDraftBody& body,
    std::string* error_out) {
    for (std::size_t ordinal = 0; ordinal < body.witnesses.size(); ++ordinal) {
        Statement insert(db, "INSERT INTO au_predicate_witness_v2(predicate_definition_revision_id,witness_ordinal,witness_name,value_builtin_type,value_schema_canonical_id,value_schema_revision,value_schema_sha256) VALUES(?,?,?,?,?,?,?)");
        if (!insert) { Error(db, error_out, "prepare witness insert failed"); return false; }
        sqlite3_bind_int64(insert.value, 1, revision_id);
        sqlite3_bind_int64(insert.value, 2, static_cast<sqlite3_int64>(ordinal));
        BindText(insert.value, 3, body.witnesses[ordinal].name);
        BindType(insert.value, 4, body.witnesses[ordinal].value_type);
        if (sqlite3_step(insert.value) != SQLITE_DONE) {
            Error(db, error_out, "witness insert failed"); return false;
        }
    }
    for (std::size_t ordinal = 0; ordinal < body.expression.size(); ++ordinal) {
        const auto& node = body.expression[ordinal];
        Statement insert(db, "INSERT INTO au_predicate_expression_node_v2(predicate_definition_revision_id,node_ordinal,node_kind,result_builtin_type,result_schema_canonical_id,result_schema_revision,result_schema_sha256,witness_ordinal,literal_kind,literal_integer,literal_real,literal_text,literal_blob,reducer_canonical_id,reducer_revision,reducer_sha256,source_label) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        if (!insert) { Error(db, error_out, "prepare expression insert failed"); return false; }
        sqlite3_bind_int64(insert.value, 1, revision_id);
        sqlite3_bind_int64(insert.value, 2, static_cast<sqlite3_int64>(ordinal));
        BindText(insert.value, 3, NodeKind(node.kind));
        BindType(insert.value, 4, node.result_type);
        if (node.witness_index) sqlite3_bind_int64(insert.value, 8, *node.witness_index);
        if (node.literal && !BindLiteral(insert.value, 9, *node.literal)) {
            if (error_out) *error_out = "unsupported predicate literal";
            return false;
        }
        if (node.reducer) {
            BindText(insert.value, 14, node.reducer->canonical_id);
            sqlite3_bind_int64(insert.value, 15, node.reducer->version);
            BindText(insert.value, 16, node.reducer->signature_hash.ToHex());
        }
        BindText(insert.value, 17, node.source_label);
        if (sqlite3_step(insert.value) != SQLITE_DONE) {
            Error(db, error_out, "expression node insert failed"); return false;
        }
        for (std::size_t edge = 0; edge < node.operands.size(); ++edge) {
            Statement edge_insert(db, "INSERT INTO au_predicate_expression_edge_v2 VALUES(?,?,?,?)");
            sqlite3_bind_int64(edge_insert.value, 1, revision_id);
            sqlite3_bind_int64(edge_insert.value, 2, static_cast<sqlite3_int64>(ordinal));
            sqlite3_bind_int64(edge_insert.value, 3, static_cast<sqlite3_int64>(edge));
            sqlite3_bind_int64(edge_insert.value, 4, static_cast<sqlite3_int64>(node.operands[edge]));
            if (sqlite3_step(edge_insert.value) != SQLITE_DONE) {
                Error(db, error_out, "expression edge insert failed"); return false;
            }
        }
    }
    return true;
}

PredicateAuthoringRevisionReceipt DefinitionReceipt(
    sqlite3* db,
    std::int64_t revision_id,
    bool identity_created,
    bool semantic_changed) {
    PredicateAuthoringRevisionReceipt receipt{};
    Statement statement(db,
        "SELECT d.predicate_definition_id,d.stable_key,r.predicate_definition_revision_id,"
        "r.revision_number,r.revision_state,r.semantic_sha256 "
        "FROM au_predicate_definition_revision_v2 r JOIN au_predicate_definition_v2 d "
        "ON d.predicate_definition_id=r.predicate_definition_id "
        "WHERE r.predicate_definition_revision_id=?");
    sqlite3_bind_int64(statement.value, 1, revision_id);
    if (sqlite3_step(statement.value) == SQLITE_ROW) {
        receipt.parent_id = sqlite3_column_int64(statement.value, 0);
        receipt.stable_key = Text(statement.value, 1);
        receipt.revision_id = sqlite3_column_int64(statement.value, 2);
        receipt.revision_number = sqlite3_column_int(statement.value, 3);
        receipt.revision_state = Text(statement.value, 4);
        receipt.semantic_sha256 = Text(statement.value, 5);
        receipt.identity_created = identity_created;
        receipt.semantic_changed = semantic_changed;
    }
    return receipt;
}

bool LookupCreationRequest(
    sqlite3* db,
    std::string_view operation,
    std::string_view request_key,
    std::string_view request_hash,
    std::int64_t* revision_id_out,
    std::string* error_out) {
    Statement statement(db, "SELECT request_sha256,revision_id FROM au_predicate_authoring_request WHERE operation_kind=? AND creation_request_key=?");
    if (!statement) {
        Error(db, error_out, "predicate authoring request lookup preparation failed");
        if (revision_id_out) *revision_id_out = -1;
        return true;
    }
    BindText(statement.value, 1, operation);
    BindText(statement.value, 2, request_key);
    if (sqlite3_step(statement.value) != SQLITE_ROW) return false;
    if (Text(statement.value, 0) != request_hash) {
        if (error_out) *error_out = "predicate authoring request key was reused with different content";
        if (revision_id_out) *revision_id_out = -1;
        return true;
    }
    if (revision_id_out) *revision_id_out = sqlite3_column_int64(statement.value, 1);
    return true;
}

bool InsertCreationRequest(
    sqlite3* db,
    std::string_view operation,
    std::string_view request_key,
    std::string_view request_hash,
    std::int64_t parent_id,
    std::int64_t revision_id,
    types::UtcTimePoint when,
    std::string* error_out) {
    Statement statement(db, "INSERT INTO au_predicate_authoring_request(operation_kind,creation_request_key,request_sha256,parent_id,revision_id,created_at_utc) VALUES(?,?,?,?,?,?)");
    if (!statement) {
        Error(db, error_out, "predicate authoring request ledger preparation failed");
        return false;
    }
    BindText(statement.value, 1, operation);
    BindText(statement.value, 2, request_key);
    BindText(statement.value, 3, request_hash);
    sqlite3_bind_int64(statement.value, 4, parent_id);
    sqlite3_bind_int64(statement.value, 5, revision_id);
    sqlite3_bind_int64(statement.value, 6, Millis(when));
    if (sqlite3_step(statement.value) == SQLITE_DONE) return true;
    Error(db, error_out, "predicate authoring request ledger insert failed");
    return false;
}

bool SqliteAuthoringDb::AbandonPredicateDefinitionDraftV2(
    std::int64_t revision_id, std::string* error_out) {
    Statement statement(db_,"DELETE FROM au_predicate_definition_revision_v2 WHERE predicate_definition_revision_id=? AND revision_state='DRAFT'");
    if(!statement){Error(db_,error_out,"prepare failed");return false;}
    sqlite3_bind_int64(statement.value,1,revision_id);
    if(sqlite3_step(statement.value)!=SQLITE_DONE||sqlite3_changes(db_)!=1){
        if(error_out)*error_out="predicate definition revision is not an abandonable draft";return false;}
    return true;
}

bool SqliteAuthoringDb::UpdatePredicateAuthoringMetadata(
    const UpdatePredicateAuthoringMetadataCommand& command,
    bool* changed_out,
    std::string* error_out) {
    if (changed_out) *changed_out = false;
    if (!db_ || command.parent_id <= 0 || command.name.empty()) {
        if (error_out) *error_out = "predicate metadata update is invalid";
        return false;
    }
    const char* table = nullptr;
    const char* id_column = nullptr;
    switch (command.object_kind) {
    case PredicateAuthoringObjectKind::Definition:
        table = "au_predicate_definition_v2"; id_column = "predicate_definition_id"; break;
    case PredicateAuthoringObjectKind::ExecutionBinding:
        table = "au_predicate_execution_binding"; id_column = "predicate_execution_binding_id"; break;
    case PredicateAuthoringObjectKind::Group:
        table = "au_predicate_group"; id_column = "predicate_group_id"; break;
    }
    const std::string query = std::string("SELECT name,description FROM ") + table + " WHERE " + id_column + "=?";
    Statement existing(db_, query.c_str());
    sqlite3_bind_int64(existing.value, 1, command.parent_id);
    if (sqlite3_step(existing.value) != SQLITE_ROW) {
        if (error_out) *error_out = "predicate authoring identity was not found";
        return false;
    }
    if (Text(existing.value, 0) == command.name && Text(existing.value, 1) == command.description) return true;
    const std::string sql = std::string("UPDATE ") + table + " SET name=?,description=?,updated_at_utc=? WHERE " + id_column + "=?";
    Statement update(db_, sql.c_str());
    BindText(update.value, 1, command.name);
    BindText(update.value, 2, command.description);
    sqlite3_bind_int64(update.value, 3, Millis(command.updated_at_utc));
    sqlite3_bind_int64(update.value, 4, command.parent_id);
    if (sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        Error(db_, error_out, "predicate metadata update failed"); return false;
    }
    if (changed_out) *changed_out = true;
    return true;
}


} // namespace savor::db
