cmake_minimum_required(VERSION 3.25)

project(SOASim LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(ProjectOptions)
include(Dependencies)
include(GeneratedSources)

soasim_apply_project_options()
soasim_setup_dependencies()

add_subdirectory(SimCore)
add_subdirectory(SimCoreWorker)
