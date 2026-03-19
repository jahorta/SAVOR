file(GLOB_RECURSE simcore_sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
)
list(APPEND simcore_sources "${CMAKE_SOURCE_DIR}/third-party/sqlite/include/sqlite3.c")

soasim_generate_simcore_sources(simcore_reflect_header simcore_migrations_header)

add_library(simcore STATIC
  ${simcore_sources}
  ${simcore_reflect_header}
  ${simcore_migrations_header}
)

target_link_libraries(simcore
  PUBLIC
    soasim_thirdparty
)

target_include_directories(simcore
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Memory/Soa"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/SimCore"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/SimCore/Core/Memory/Soa"
)

target_compile_definitions(simcore
  PRIVATE
    "$<$<CONFIG:Debug>:_DEBUG>"
    "$<$<NOT:$<CONFIG:Debug>>:NDEBUG>"
    _LIB
    NOMINMAX
)

target_compile_options(simcore
  PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W3;/sdl;/permissive->"
)

set_target_properties(simcore PROPERTIES
  OUTPUT_NAME "SimCore"
)
