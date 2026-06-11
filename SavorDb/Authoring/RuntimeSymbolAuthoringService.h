#pragma once

#include <cstdint>
#include <string>

#include <sqlite3.h>

#include "../../SavorCore/Runner/Symbols/RuntimeSymbolRegistry.h"

namespace savor::db {

struct RuntimeSymbolPackImportResult {
    std::int64_t runtime_symbol_pack_id = 0;
    int context_symbol_count = 0;
    int address_symbol_count = 0;
    int breakpoint_symbol_count = 0;
};

class RuntimeSymbolAuthoringService {
public:
    explicit RuntimeSymbolAuthoringService(sqlite3* db) : db_(db) {}

    bool ImportJson(
        const std::string& json_text,
        RuntimeSymbolPackImportResult* result_out = nullptr,
        std::string* error_out = nullptr) const;

    bool ExportJson(
        const std::string& pack_id,
        int schema_version,
        std::string& json_out,
        std::string* error_out = nullptr) const;

    bool LoadCustomSymbols(
        savor::symbols::RuntimeSymbolRegistry& registry,
        std::string* error_out = nullptr) const;

private:
    sqlite3* db_ = nullptr;
};

} // namespace savor::db
