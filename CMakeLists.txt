cmake_minimum_required(VERSION 3.15)

project(
    gbt_flat_map
    VERSION 0.1.0
    DESCRIPTION "Header-only flat map implementations"
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(flat_map INTERFACE)
add_library(gbt::flat_map ALIAS flat_map)

target_sources(flat_map
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/gbt/elastic_flat_map>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/gbt/elastic_flat_map.hpp>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/gbt/funnel_flat_map>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/gbt/funnel_flat_map.hpp>
)

target_include_directories(flat_map
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(flat_map
    INTERFACE
        cxx_std_17
)

option(
    GBT_FLAT_MAP_BUILD_BENCHMARK
    "Build the gbt flat map benchmark executable"
    OFF
)

if(GBT_FLAT_MAP_BUILD_BENCHMARK)
    add_subdirectory(benchmark)
endif()

set(
    GBT_FLAT_MAP_INSTALL_CMAKEDIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/gbt_flat_map"
    CACHE STRING
    "CMake package config install directory"
)

install(
    TARGETS flat_map
    EXPORT gbt_flat_mapTargets
)

install(
    DIRECTORY include/
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
    EXPORT gbt_flat_mapTargets
    NAMESPACE gbt::
    FILE gbt_flat_mapTargets.cmake
    DESTINATION "${GBT_FLAT_MAP_INSTALL_CMAKEDIR}"
)

configure_package_config_file(
    cmake/gbt_flat_mapConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/gbt_flat_mapConfig.cmake"
    INSTALL_DESTINATION "${GBT_FLAT_MAP_INSTALL_CMAKEDIR}"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/gbt_flat_mapConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/gbt_flat_mapConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/gbt_flat_mapConfigVersion.cmake"
    DESTINATION "${GBT_FLAT_MAP_INSTALL_CMAKEDIR}"
)
