if(NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_FILE)
  message(FATAL_ERROR "SOURCE_DIR and OUTPUT_FILE are required")
endif()

file(GLOB migration_files LIST_DIRECTORIES false "${SOURCE_DIR}/*.sql")
list(SORT migration_files)

get_filename_component(output_dir "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")

set(content "// generated\n#pragma once\n#include <string>\n#include <utility>\n#include <vector>\nnamespace simcore{namespace db{ static inline const std::vector<std::pair<std::string,std::string>> kEmbeddedMigrations = {\n")

foreach(migration_file IN LISTS migration_files)
  file(READ "${migration_file}" migration_sql)
  string(REPLACE ")SQL\"" ")SQL\\\"" migration_sql "${migration_sql}")
  get_filename_component(migration_name "${migration_file}" NAME)
  string(APPEND content "  {\"${migration_name}\", R\"SQL(${migration_sql})SQL\"},\n")
endforeach()

string(APPEND content "}; }} // ns\n")
file(WRITE "${OUTPUT_FILE}" "${content}")
