function(soasim_setup_dependencies)
  set(SOASIM_DOLPHIN_ROOT "${CMAKE_SOURCE_DIR}/third-party/dolphin-2506a" CACHE PATH "Path to the vendored Dolphin root")
  set(SOASIM_GTEST_ROOT "${CMAKE_SOURCE_DIR}/third-party/googletest-1.17.0" CACHE PATH "Path to the vendored GoogleTest root")
  set(SOASIM_SQLITE_ROOT "${CMAKE_SOURCE_DIR}/third-party/sqlite" CACHE PATH "Path to the vendored SQLite root")

  add_library(soasim_thirdparty INTERFACE)
  target_include_directories(soasim_thirdparty INTERFACE
    "${SOASIM_SQLITE_ROOT}/include"
    "${SOASIM_DOLPHIN_ROOT}/include/Source"
    "${SOASIM_DOLPHIN_ROOT}/include/Source/Core"
    "${SOASIM_DOLPHIN_ROOT}/include/Externals/fmt/fmt/include"
    "${SOASIM_DOLPHIN_ROOT}/include/Externals/lz4/lz4/lib"
    "${SOASIM_DOLPHIN_ROOT}/include/Externals/mbedtls/include"
    "${SOASIM_DOLPHIN_ROOT}/include/Externals/zlib-ng"
    "${SOASIM_GTEST_ROOT}/googletest/include"
    "${SOASIM_GTEST_ROOT}/googlemock/include"
  )
  target_compile_features(soasim_thirdparty INTERFACE cxx_std_20)
  if(MSVC)
    target_compile_options(soasim_thirdparty INTERFACE /Zc:preprocessor)
  endif()

  add_library(soasim_linked_libs INTERFACE)
  target_link_directories(soasim_linked_libs INTERFACE
    "${SOASIM_DOLPHIN_ROOT}/lib/x64/$<CONFIG>"
  )
  target_link_libraries(soasim_linked_libs INTERFACE
    "$<$<CXX_COMPILER_ID:MSVC>:$(CoreLibraryDependencies)>"
    build_pch.lib
    core.lib
    discio.lib
    common.lib
    videocommon.lib
    videod3d.lib
    videod3d12.lib
    videod3dcommon.lib
    videonull.lib
    videoogl.lib
    videosoftware.lib
    imgui.lib
    implot.lib
    audiocommon.lib
    inputcommon.lib
    uicommon.lib
    mbedcrypto.lib
    mbedtls.lib
    mbedx509.lib
    enet.lib
    FatFs.lib
    bdisasm.lib
    sfml-network.lib
    sfml-system.lib
    usb.lib
    spirv_cross.lib
    FreeSurround.lib
    spng.lib
    hidapi.lib
    lz4.lib
    lzo2.lib
    pugixml.lib
    zstd.lib
    bzip2.lib
    lzma.lib
    tinygltf.lib
    glslang.lib
    xxhash.lib
    opengl32.lib
    "$<$<CONFIG:Debug>:libcurl-d.lib>"
    "$<$<CONFIG:Debug>:zlibstaticd.lib>"
    "$<$<CONFIG:Debug>:fmtd.lib>"
    "$<$<NOT:$<CONFIG:Debug>>:libcurl.lib>"
    "$<$<NOT:$<CONFIG:Debug>>:zlibstatic.lib>"
    "$<$<NOT:$<CONFIG:Debug>>:fmt.lib>"
  )
endfunction()
