function(soasim_generate_simcore_sources out_reflect out_migrations)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)

  set(simcore_root "${CMAKE_SOURCE_DIR}/SimCore")
  set(generated_root "${CMAKE_CURRENT_BINARY_DIR}/generated/SimCore")
  set(reflect_output "${generated_root}/Core/Memory/Soa/SoaStructs.reflect.h")
  set(migrations_output "${generated_root}/GeneratedMigrations.h")
  file(MAKE_DIRECTORY "${generated_root}/Core/Memory/Soa")

  add_custom_command(
    OUTPUT "${reflect_output}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${simcore_root}/Core/Memory/Soa/SoaStructs.reflect.h"
            "${reflect_output}"
    COMMAND ${Python3_EXECUTABLE}
            "${simcore_root}/Utils/soa_struct_reflector.py"
            "${simcore_root}/Core/Memory/Soa/SoaStructs.h"
            "${reflect_output}"
    DEPENDS
      "${simcore_root}/Utils/soa_struct_reflector.py"
      "${simcore_root}/Core/Memory/Soa/SoaStructs.h"
    COMMENT "Generating SoaStructs.reflect.h"
    VERBATIM
  )

  file(GLOB simcore_migration_sql CONFIGURE_DEPENDS "${simcore_root}/DB/migration/*.sql")
  add_custom_command(
    OUTPUT "${migrations_output}"
    COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR="${simcore_root}/DB/migration"
            -DOUTPUT_FILE="${migrations_output}"
            -P "${CMAKE_SOURCE_DIR}/cmake/GenerateEmbeddedMigrations.cmake"
    DEPENDS
      ${simcore_migration_sql}
      "${CMAKE_SOURCE_DIR}/cmake/GenerateEmbeddedMigrations.cmake"
    COMMENT "Generating embedded migration header"
    VERBATIM
  )

  set(${out_reflect} "${reflect_output}" PARENT_SCOPE)
  set(${out_migrations} "${migrations_output}" PARENT_SCOPE)
endfunction()
