add_executable(simcore_worker
  SimCoreWorker.cpp
)

target_link_libraries(simcore_worker
  PRIVATE
    simcore
    soasim_linked_libs
)

target_include_directories(simcore_worker
  PRIVATE
    "${CMAKE_SOURCE_DIR}/SimCore"
)

target_compile_definitions(simcore_worker
  PRIVATE
    "$<$<CONFIG:Debug>:_DEBUG>"
    "$<$<NOT:$<CONFIG:Debug>>:NDEBUG>"
    _CONSOLE
    NOMINMAX
    WIN32
    _CRT_SECURE_NO_WARNINGS
)

target_compile_options(simcore_worker
  PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/W3;/sdl;/permissive->"
)

set_target_properties(simcore_worker PROPERTIES
  OUTPUT_NAME "SimCoreWorker"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/x64/$<CONFIG>"
)
