cmake_minimum_required(VERSION 3.24)
project(eglantine_nx2_core_check LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
file(READ "${EDEN_SOURCE}/cpmfile.json" packages)
string(JSON catch_version GET "${packages}" catch2 version)
string(JSON catch_hash GET "${packages}" catch2 hash)
set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(Catch2
    URL "https://github.com/catchorg/Catch2/archive/refs/tags/${catch_version}.tar.gz"
    URL_HASH "SHA512=${catch_hash}")
FetchContent_MakeAvailable(Catch2)
file(GLOB ampere_tests CONFIGURE_DEPENDS "${EDEN_SOURCE}/src/tests/video_core/ampere_*.cpp")
# These integration tests link the complete shader compiler and run in upstream tests.
# This host check exercises the header-only native capture/lowering and memory helpers.
list(FILTER ampere_tests EXCLUDE REGEX
    "ampere_(bindless_tex|sured|async_copy|fast_fusion_logged|geometry|logged_numeric|logged_textures|starfox_test10|tennis_tessellation)\\.cpp$")
# The Lichii core's own Switch 2 mapping test (header-only); the earlier readback, command-sync
# and sparse-tile suites went with the core implementation they tested.
add_executable(nx2-core-check ${ampere_tests}
    logging.cpp
    "${EDEN_SOURCE}/src/tests/video_core/nx2_gpu_mapping.cpp")
file(GLOB boost_includes "${DEPENDENCY_SOURCE}/.cache/cpm/boost/*/libs/*/include")
# Eden's pinned Vulkan headers, not whatever the host has installed.
file(GLOB vulkan_includes "${DEPENDENCY_SOURCE}/.cache/cpm/vulkanheaders/*/include")
if(NOT vulkan_includes)
    message(FATAL_ERROR "Eden's Vulkan headers are not in ${DEPENDENCY_SOURCE}/.cache/cpm/vulkanheaders")
endif()
target_include_directories(nx2-core-check PRIVATE "${EDEN_SOURCE}/src" "${FMT_INCLUDE}" ${boost_includes}
    ${vulkan_includes})
target_link_libraries(nx2-core-check PRIVATE Catch2::Catch2WithMain)
