#pragma once
#include "DbEnv.h"
#include <string>
#include <cstdint>

namespace simcore {
	namespace db {

		// Applies all migrations in `dir`, sorted by filename (e.g., 0001.sql -> 0002.sql).
		// Each file is executed as a single SQL script (multiple statements allowed).
		// Returns the final schema_version after applying migrations.
		int ApplyMigrationsFromDir(DbEnv& env, const std::string& dir);

		// Reads current schema_version (0 if table missing/empty).
		int GetCurrentSchemaVersion(DbEnv& env);

	} // namespace db
} // namespace simcore
