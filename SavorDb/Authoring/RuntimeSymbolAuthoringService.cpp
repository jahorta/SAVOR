#include "RuntimeSymbolAuthoringService.h"

#include <optional>
#include <sstream>
#include <string_view>

#include "../../SavorCore/Utils/Hash.h"

namespace savor::db {

namespace {

struct Statement {
    ~Statement() {
        if (st != nullptr) sqlite3_finalize(st);
    }
    sqlite3_stmt* st = nullptr;
};

bool Exec(sqlite3* db, const char* sql, std::string* error_out)
{
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (error_out) *error_out = err != nullptr ? err : sqlite3_errmsg(db);
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool Prepare(sqlite3* db, const char* sql, Statement& st, std::string* error_out)
{
    if (sqlite3_prepare_v2(db, sql, -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

bool StepDone(sqlite3* db, sqlite3_stmt* st, std::string* error_out)
{
    const int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

std::string ColumnText(sqlite3_stmt* st, int col)
{
    const auto* text = sqlite3_column_text(st, col);
    return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
}

bool ScalarText(sqlite3* db, const char* sql, const std::string& arg, std::string& out, std::string* error_out)
{
    Statement st;
    if (!Prepare(db, sql, st, error_out)) return false;
    sqlite3_bind_text(st.st, 1, arg.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(st.st);
    if (rc != SQLITE_ROW) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    out = ColumnText(st.st, 0);
    return true;
}

bool ScalarInt64(sqlite3* db, const char* sql, std::int64_t& out, std::string* error_out)
{
    Statement st;
    if (!Prepare(db, sql, st, error_out)) return false;
    const int rc = sqlite3_step(st.st);
    if (rc != SQLITE_ROW) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    out = sqlite3_column_int64(st.st, 0);
    return true;
}

std::string EscapeJson(std::string_view value)
{
    std::ostringstream out;
    out << '"';
    for (const char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default: out << ch; break;
        }
    }
    out << '"';
    return out.str();
}

std::string RegionToDb(addr::Region region)
{
    switch (region) {
    case addr::Region::MEM1: return "MEM1";
    case addr::Region::MEM2: return "MEM2";
    case addr::Region::DERIVED: return "DERIVED";
    }
    return "";
}

std::optional<addr::Region> ParseRegion(std::string_view value)
{
    if (value == "MEM1") return addr::Region::MEM1;
    if (value == "MEM2") return addr::Region::MEM2;
    if (value == "DERIVED") return addr::Region::DERIVED;
    return std::nullopt;
}

std::optional<uint32_t> ParseAddressLiteral(std::string_view value)
{
    if (value.empty()) return std::nullopt;
    uint64_t out = 0;
    size_t pos = 0;
    const bool hex = value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X');
    if (hex) pos = 2;
    for (; pos < value.size(); ++pos) {
        const char c = value[pos];
        uint32_t digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
        else if (hex && c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(10 + c - 'a');
        else if (hex && c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(10 + c - 'A');
        else return std::nullopt;
        out = hex ? ((out << 4) | digit) : (out * 10 + digit);
        if (out > 0xFFFFFFFFull) return std::nullopt;
    }
    return static_cast<uint32_t>(out);
}

int CountRows(sqlite3* db, const char* sql, std::int64_t pack_db_id)
{
    Statement st;
    if (sqlite3_prepare_v2(db, sql, -1, &st.st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st.st, 1, pack_db_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return 0;
    return sqlite3_column_int(st.st, 0);
}

bool ValidateJsonString(sqlite3* db, const std::string& json, const char* path, std::string& out, std::string* error_out)
{
    std::string sql = "SELECT COALESCE(json_extract(?1, '" + std::string(path) + "'), '')";
    if (!ScalarText(db, sql.c_str(), json, out, error_out)) return false;
    if (out.empty()) {
        if (error_out) *error_out = "missing required JSON field: " + std::string(path);
        return false;
    }
    return true;
}

bool ValidateJsonVersion(sqlite3* db, const std::string& json, std::string* error_out)
{
    std::string schema;
    if (!ValidateJsonString(db, json, "$.schema", schema, error_out)) return false;
    if (schema != "savor.runtime-symbol-pack") {
        if (error_out) *error_out = "unsupported runtime symbol pack schema";
        return false;
    }
    Statement st;
    if (!Prepare(db, "SELECT COALESCE(json_extract(?1, '$.version'), 0)", st, error_out)) return false;
    sqlite3_bind_text(st.st, 1, json.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    if (sqlite3_column_int(st.st, 0) != 1) {
        if (error_out) *error_out = "unsupported runtime symbol pack version";
        return false;
    }
    return true;
}

bool ValidateJsonArray(sqlite3* db, const std::string& json, const char* path, std::string* error_out)
{
    Statement st;
    std::string sql = "SELECT COALESCE(json_type(?1, '" + std::string(path) + "'), '')";
    if (!Prepare(db, sql.c_str(), st, error_out)) return false;
    sqlite3_bind_text(st.st, 1, json.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    const std::string type = ColumnText(st.st, 0);
    if (!type.empty() && type != "array") {
        if (error_out) *error_out = "runtime symbol pack field must be an array: " + std::string(path);
        return false;
    }
    return true;
}

bool HasMissingBreakpointAddress(
    sqlite3* db,
    std::int64_t pack_db_id,
    const savor::symbols::RuntimeSymbolRegistry& builtins,
    std::string* error_out)
{
    Statement st;
    const char* sql =
        "SELECT b.address_id "
        "FROM au_breakpoint_symbol b "
        "LEFT JOIN au_address_symbol a "
        "  ON a.runtime_symbol_pack_id=b.runtime_symbol_pack_id AND a.stable_id=b.address_id "
        "WHERE b.runtime_symbol_pack_id=?1 AND a.address_symbol_id IS NULL;";
    if (!Prepare(db, sql, st, error_out)) return true;
    sqlite3_bind_int64(st.st, 1, pack_db_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        const std::string address_id = ColumnText(st.st, 0);
        if (builtins.FindAddress(address_id) == nullptr) {
            if (error_out) *error_out = "breakpoint references missing address symbol: " + address_id;
            return true;
        }
    }
    return false;
}

} // namespace

bool RuntimeSymbolAuthoringService::ImportJson(
    const std::string& json_text,
    RuntimeSymbolPackImportResult* result_out,
    std::string* error_out) const
{
    if (db_ == nullptr) {
        if (error_out) *error_out = "authoring db is null";
        return false;
    }
    if (!ValidateJsonVersion(db_, json_text, error_out)) return false;
    if (!ValidateJsonArray(db_, json_text, "$.context_keys", error_out)) return false;
    if (!ValidateJsonArray(db_, json_text, "$.addresses", error_out)) return false;
    if (!ValidateJsonArray(db_, json_text, "$.breakpoints", error_out)) return false;

    std::string pack_id;
    if (!ValidateJsonString(db_, json_text, "$.id", pack_id, error_out)) return false;
    if (pack_id.rfind("user.pack.", 0) != 0) {
        if (error_out) *error_out = "runtime symbol pack id must start with user.pack.";
        return false;
    }

    const std::string content_hash = hash::sha256(
        reinterpret_cast<const std::uint8_t*>(json_text.data()),
        json_text.size());

    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) return false;
    auto rollback = [&]() { sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr); };

    Statement upsert;
    const char* upsert_sql =
        "INSERT INTO au_runtime_symbol_pack("
        "pack_id,schema_name,schema_version,name,description,content_hash,created_at_utc,imported_at_utc) "
        "VALUES(?1,'savor.runtime-symbol-pack',1,"
        "COALESCE(json_extract(?2,'$.name'),?1),json_extract(?2,'$.description'),?3,"
        "CAST(strftime('%s','now') AS INTEGER)*1000,CAST(strftime('%s','now') AS INTEGER)*1000) "
        "ON CONFLICT(pack_id,schema_version) DO UPDATE SET "
        "name=excluded.name,description=excluded.description,content_hash=excluded.content_hash,"
        "imported_at_utc=excluded.imported_at_utc;";
    if (!Prepare(db_, upsert_sql, upsert, error_out)) { rollback(); return false; }
    sqlite3_bind_text(upsert.st, 1, pack_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(upsert.st, 2, json_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(upsert.st, 3, content_hash.c_str(), -1, SQLITE_TRANSIENT);
    if (!StepDone(db_, upsert.st, error_out)) { rollback(); return false; }

    std::int64_t pack_db_id = 0;
    {
        Statement st;
        if (!Prepare(db_, "SELECT runtime_symbol_pack_id FROM au_runtime_symbol_pack WHERE pack_id=?1 AND schema_version=1;", st, error_out)) {
            rollback(); return false;
        }
        sqlite3_bind_text(st.st, 1, pack_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st.st) != SQLITE_ROW) {
            if (error_out) *error_out = "failed to read runtime symbol pack row";
            rollback(); return false;
        }
        pack_db_id = sqlite3_column_int64(st.st, 0);
    }

    const char* delete_sql[] = {
        "DELETE FROM au_breakpoint_symbol WHERE runtime_symbol_pack_id=?1;",
        "DELETE FROM au_address_symbol WHERE runtime_symbol_pack_id=?1;",
        "DELETE FROM au_context_symbol WHERE runtime_symbol_pack_id=?1;",
    };
    for (const char* sql : delete_sql) {
        Statement st;
        if (!Prepare(db_, sql, st, error_out)) { rollback(); return false; }
        sqlite3_bind_int64(st.st, 1, pack_db_id);
        if (!StepDone(db_, st.st, error_out)) { rollback(); return false; }
    }

    struct InsertSpec { const char* sql; const char* json_path; };
    const InsertSpec inserts[] = {
        {
            "INSERT INTO au_context_symbol(runtime_symbol_pack_id,stable_id,name,value_type,description,ordinal) "
            "SELECT ?1,json_extract(value,'$.id'),COALESCE(json_extract(value,'$.name'),json_extract(value,'$.id')),"
            "json_extract(value,'$.type'),json_extract(value,'$.description'),CAST(key AS INTEGER) "
            "FROM json_each(?2,'$.context_keys');",
            "$.context_keys"
        },
        {
            "INSERT INTO au_breakpoint_symbol(runtime_symbol_pack_id,stable_id,name,address_id,kind,enabled,domain,notes,ordinal) "
            "SELECT ?1,json_extract(value,'$.id'),COALESCE(json_extract(value,'$.name'),json_extract(value,'$.id')),"
            "json_extract(value,'$.address_id'),COALESCE(json_extract(value,'$.kind'),'execute'),"
            "COALESCE(json_extract(value,'$.enabled'),1),json_extract(value,'$.domain'),json_extract(value,'$.notes'),CAST(key AS INTEGER) "
            "FROM json_each(?2,'$.breakpoints');",
            "$.breakpoints"
        },
    };
    for (const auto& spec : inserts) {
        Statement st;
        if (!Prepare(db_, spec.sql, st, error_out)) { rollback(); return false; }
        sqlite3_bind_int64(st.st, 1, pack_db_id);
        sqlite3_bind_text(st.st, 2, json_text.c_str(), -1, SQLITE_TRANSIENT);
        if (!StepDone(db_, st.st, error_out)) { rollback(); return false; }
    }

    {
        Statement read;
        const char* read_sql =
            "SELECT CAST(key AS INTEGER),json_extract(value,'$.id'),"
            "COALESCE(json_extract(value,'$.name'),json_extract(value,'$.id')),"
            "json_extract(value,'$.region'),json_extract(value,'$.address'),"
            "json_extract(value,'$.width'),json_extract(value,'$.type'),json_extract(value,'$.notes') "
            "FROM json_each(?1,'$.addresses');";
        if (!Prepare(db_, read_sql, read, error_out)) { rollback(); return false; }
        sqlite3_bind_text(read.st, 1, json_text.c_str(), -1, SQLITE_TRANSIENT);

        Statement insert;
        const char* insert_sql =
            "INSERT INTO au_address_symbol(runtime_symbol_pack_id,stable_id,name,region,base_address,width,value_type,notes,ordinal) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);";
        if (!Prepare(db_, insert_sql, insert, error_out)) { rollback(); return false; }

        while (sqlite3_step(read.st) == SQLITE_ROW) {
            const std::string address_text = ColumnText(read.st, 4);
            const auto parsed_address = ParseAddressLiteral(address_text);
            if (!parsed_address.has_value()) {
                if (error_out) *error_out = "invalid address literal: " + address_text;
                rollback(); return false;
            }
            const std::string stable_id = ColumnText(read.st, 1);
            const std::string name = ColumnText(read.st, 2);
            const std::string region = ColumnText(read.st, 3);
            if (stable_id.empty() || stable_id.rfind("user.addr.", 0) != 0) {
                if (error_out) *error_out = "address symbol id must start with user.addr.";
                rollback(); return false;
            }
            if (name.empty()) {
                if (error_out) *error_out = "address symbol name is required";
                rollback(); return false;
            }
            if (!ParseRegion(region).has_value()) {
                if (error_out) *error_out = "invalid address symbol region: " + region;
                rollback(); return false;
            }
            if (sqlite3_column_type(read.st, 6) != SQLITE_NULL) {
                const std::string type = ColumnText(read.st, 6);
                if (!savor::symbols::ParseContextValueType(type).has_value()) {
                    if (error_out) *error_out = "invalid address symbol type: " + type;
                    rollback(); return false;
                }
            }

            sqlite3_reset(insert.st);
            sqlite3_clear_bindings(insert.st);
            sqlite3_bind_int64(insert.st, 1, pack_db_id);
            sqlite3_bind_text(insert.st, 2, stable_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert.st, 3, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert.st, 4, region.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(insert.st, 5, *parsed_address);
            if (sqlite3_column_type(read.st, 5) == SQLITE_NULL) sqlite3_bind_null(insert.st, 6);
            else sqlite3_bind_int(insert.st, 6, sqlite3_column_int(read.st, 5));
            if (sqlite3_column_type(read.st, 6) == SQLITE_NULL) sqlite3_bind_null(insert.st, 7);
            else sqlite3_bind_text(insert.st, 7, ColumnText(read.st, 6).c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_column_type(read.st, 7) == SQLITE_NULL) sqlite3_bind_null(insert.st, 8);
            else sqlite3_bind_text(insert.st, 8, ColumnText(read.st, 7).c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(insert.st, 9, sqlite3_column_int(read.st, 0));
            if (!StepDone(db_, insert.st, error_out)) { rollback(); return false; }
        }
    }

    const auto builtins = savor::symbols::RuntimeSymbolRegistry::BuiltIns();
    if (HasMissingBreakpointAddress(db_, pack_db_id, builtins, error_out)) { rollback(); return false; }

    if (!Exec(db_, "COMMIT;", error_out)) { rollback(); return false; }

    if (result_out != nullptr) {
        result_out->runtime_symbol_pack_id = pack_db_id;
        result_out->context_symbol_count = CountRows(db_, "SELECT COUNT(*) FROM au_context_symbol WHERE runtime_symbol_pack_id=?1;", pack_db_id);
        result_out->address_symbol_count = CountRows(db_, "SELECT COUNT(*) FROM au_address_symbol WHERE runtime_symbol_pack_id=?1;", pack_db_id);
        result_out->breakpoint_symbol_count = CountRows(db_, "SELECT COUNT(*) FROM au_breakpoint_symbol WHERE runtime_symbol_pack_id=?1;", pack_db_id);
    }
    return true;
}

bool RuntimeSymbolAuthoringService::ExportJson(
    const std::string& pack_id,
    int schema_version,
    std::string& json_out,
    std::string* error_out) const
{
    if (db_ == nullptr) {
        if (error_out) *error_out = "authoring db is null";
        return false;
    }

    Statement pack;
    const char* pack_sql =
        "SELECT runtime_symbol_pack_id,name,COALESCE(description,'') "
        "FROM au_runtime_symbol_pack WHERE pack_id=?1 AND schema_version=?2;";
    if (!Prepare(db_, pack_sql, pack, error_out)) return false;
    sqlite3_bind_text(pack.st, 1, pack_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(pack.st, 2, schema_version);
    if (sqlite3_step(pack.st) != SQLITE_ROW) {
        if (error_out) *error_out = "runtime symbol pack not found";
        return false;
    }
    const std::int64_t pack_db_id = sqlite3_column_int64(pack.st, 0);
    const std::string name = ColumnText(pack.st, 1);
    const std::string description = ColumnText(pack.st, 2);

    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"savor.runtime-symbol-pack\",\n";
    out << "  \"version\": 1,\n";
    out << "  \"id\": " << EscapeJson(pack_id) << ",\n";
    out << "  \"name\": " << EscapeJson(name) << ",\n";
    out << "  \"description\": " << EscapeJson(description) << ",\n";

    auto write_rows = [&](const char* section, const char* sql, auto write_row) -> bool {
        out << "  \"" << section << "\": [\n";
        Statement st;
        if (!Prepare(db_, sql, st, error_out)) return false;
        sqlite3_bind_int64(st.st, 1, pack_db_id);
        bool first = true;
        while (sqlite3_step(st.st) == SQLITE_ROW) {
            if (!first) out << ",\n";
            first = false;
            out << "    ";
            write_row(st.st);
        }
        out << "\n  ]";
        return true;
    };

    if (!write_rows(
            "context_keys",
            "SELECT stable_id,name,value_type,COALESCE(description,'') FROM au_context_symbol WHERE runtime_symbol_pack_id=?1 ORDER BY ordinal;",
            [&](sqlite3_stmt* st) {
                out << "{\"id\":" << EscapeJson(ColumnText(st, 0))
                    << ",\"name\":" << EscapeJson(ColumnText(st, 1))
                    << ",\"type\":" << EscapeJson(ColumnText(st, 2));
                const std::string desc = ColumnText(st, 3);
                if (!desc.empty()) out << ",\"description\":" << EscapeJson(desc);
                out << "}";
            })) return false;
    out << ",\n";

    if (!write_rows(
            "addresses",
            "SELECT stable_id,name,region,base_address,width,value_type,COALESCE(notes,'') FROM au_address_symbol WHERE runtime_symbol_pack_id=?1 ORDER BY ordinal;",
            [&](sqlite3_stmt* st) {
                out << "{\"id\":" << EscapeJson(ColumnText(st, 0))
                    << ",\"name\":" << EscapeJson(ColumnText(st, 1))
                    << ",\"region\":" << EscapeJson(ColumnText(st, 2))
                    << ",\"address\":" << sqlite3_column_int64(st, 3);
                if (sqlite3_column_type(st, 4) != SQLITE_NULL) out << ",\"width\":" << sqlite3_column_int(st, 4);
                if (sqlite3_column_type(st, 5) != SQLITE_NULL) out << ",\"type\":" << EscapeJson(ColumnText(st, 5));
                const std::string notes = ColumnText(st, 6);
                if (!notes.empty()) out << ",\"notes\":" << EscapeJson(notes);
                out << "}";
            })) return false;
    out << ",\n";

    if (!write_rows(
            "breakpoints",
            "SELECT stable_id,name,address_id,kind,enabled,COALESCE(domain,''),COALESCE(notes,'') FROM au_breakpoint_symbol WHERE runtime_symbol_pack_id=?1 ORDER BY ordinal;",
            [&](sqlite3_stmt* st) {
                out << "{\"id\":" << EscapeJson(ColumnText(st, 0))
                    << ",\"name\":" << EscapeJson(ColumnText(st, 1))
                    << ",\"address_id\":" << EscapeJson(ColumnText(st, 2))
                    << ",\"kind\":" << EscapeJson(ColumnText(st, 3))
                    << ",\"enabled\":" << (sqlite3_column_int(st, 4) ? "true" : "false");
                const std::string domain = ColumnText(st, 5);
                const std::string notes = ColumnText(st, 6);
                if (!domain.empty()) out << ",\"domain\":" << EscapeJson(domain);
                if (!notes.empty()) out << ",\"notes\":" << EscapeJson(notes);
                out << "}";
            })) return false;

    out << "\n}\n";
    json_out = out.str();
    return true;
}

bool RuntimeSymbolAuthoringService::LoadCustomSymbols(
    savor::symbols::RuntimeSymbolRegistry& registry,
    std::string* error_out) const
{
    if (db_ == nullptr) {
        if (error_out) *error_out = "authoring db is null";
        return false;
    }

    {
        Statement st;
        const char* sql =
            "SELECT cs.stable_id,cs.name,cs.value_type FROM au_context_symbol cs "
            "JOIN au_runtime_symbol_pack p ON p.runtime_symbol_pack_id=cs.runtime_symbol_pack_id "
            "ORDER BY p.pack_id, cs.ordinal;";
        if (!Prepare(db_, sql, st, error_out)) return false;
        while (sqlite3_step(st.st) == SQLITE_ROW) {
            const auto type = savor::symbols::ParseContextValueType(ColumnText(st.st, 2));
            if (!type.has_value()) {
                if (error_out) *error_out = "invalid context symbol type";
                return false;
            }
            savor::symbols::ContextSymbol symbol;
            symbol.stable_id = ColumnText(st.st, 0);
            symbol.name = ColumnText(st.st, 1);
            symbol.type = *type;
            if (!registry.AddContextSymbol(std::move(symbol), error_out)) return false;
        }
    }

    {
        Statement st;
        const char* sql =
            "SELECT a.stable_id,a.name,a.region,a.base_address,a.width,a.value_type FROM au_address_symbol a "
            "JOIN au_runtime_symbol_pack p ON p.runtime_symbol_pack_id=a.runtime_symbol_pack_id "
            "ORDER BY p.pack_id, a.ordinal;";
        if (!Prepare(db_, sql, st, error_out)) return false;
        while (sqlite3_step(st.st) == SQLITE_ROW) {
            const auto region = ParseRegion(ColumnText(st.st, 2));
            if (!region.has_value()) {
                if (error_out) *error_out = "invalid address symbol region";
                return false;
            }
            savor::symbols::AddressSymbol symbol;
            symbol.stable_id = ColumnText(st.st, 0);
            symbol.name = ColumnText(st.st, 1);
            symbol.region = *region;
            symbol.base = static_cast<uint32_t>(sqlite3_column_int64(st.st, 3));
            if (sqlite3_column_type(st.st, 4) != SQLITE_NULL) symbol.width = static_cast<uint8_t>(sqlite3_column_int(st.st, 4));
            if (sqlite3_column_type(st.st, 5) != SQLITE_NULL) symbol.value_type = savor::symbols::ParseContextValueType(ColumnText(st.st, 5));
            if (sqlite3_column_type(st.st, 5) != SQLITE_NULL && !symbol.value_type.has_value()) {
                if (error_out) *error_out = "invalid address symbol type";
                return false;
            }
            if (!registry.AddAddressSymbol(std::move(symbol), error_out)) return false;
        }
    }

    {
        Statement st;
        const char* sql =
            "SELECT b.stable_id,b.name,b.address_id,b.enabled FROM au_breakpoint_symbol b "
            "JOIN au_runtime_symbol_pack p ON p.runtime_symbol_pack_id=b.runtime_symbol_pack_id "
            "ORDER BY p.pack_id, b.ordinal;";
        if (!Prepare(db_, sql, st, error_out)) return false;
        while (sqlite3_step(st.st) == SQLITE_ROW) {
            savor::symbols::BreakpointSymbol symbol;
            symbol.stable_id = ColumnText(st.st, 0);
            symbol.name = ColumnText(st.st, 1);
            symbol.address_id = ColumnText(st.st, 2);
            symbol.enabled = sqlite3_column_int(st.st, 3) != 0;
            if (!registry.AddBreakpointSymbol(std::move(symbol), error_out)) return false;
        }
    }

    return true;
}

} // namespace savor::db
