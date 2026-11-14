# SPDX-License-Identifier: GPL-3.0-or-later
# Include from the desktop CMAKE_PROJECT_yuzu_INCLUDE hook; defer this function
# until the emulator targets exist. Keep SDL audio and the desktop renderers.
function(add_linux_port_support)
    get_filename_component(EDEN_PORT_DIR "${EGLANTINE_NUP_CHECK_SOURCE}" DIRECTORY)
    get_filename_component(PORT_ROOT "${EDEN_PORT_DIR}/.." ABSOLUTE)
    set(PORT_BUILD_DIR "${CMAKE_BINARY_DIR}/linux-port")
    file(MAKE_DIRECTORY "${PORT_BUILD_DIR}")

    # Core public headers name headless/... and src/... headers from this repository.
    target_include_directories(common PUBLIC "${PORT_ROOT}")
    target_include_directories(dynarmic PRIVATE "${PORT_ROOT}")
    foreach(target core network video_core)
        if(TARGET ${target})
            target_include_directories(${target} PRIVATE "${EDEN_PORT_DIR}")
        endif()
    endforeach()
    target_link_libraries(core PRIVATE zstd::zstd)
    target_link_libraries(video_core PUBLIC OpenSSL::Crypto)

    get_target_property(common_sources common SOURCES)
    list(REMOVE_ITEM common_sources host_memory.cpp sparse_large_vector.cpp)
    set_property(TARGET common PROPERTY SOURCES "${common_sources}")
    target_sources(common PRIVATE
        "${PORT_ROOT}/src/host_memory.cpp" "${PORT_ROOT}/src/memory_pages.cpp")
    file(READ "${PROJECT_SOURCE_DIR}/src/common/host_memory.h" port_host_memory_header)
    if(port_host_memory_header MATCHES "enum class BackingMode")
        target_compile_definitions(common PRIVATE EDEN_HOST_MEMORY_BACKING_MODE=1)
    endif()
    if(port_host_memory_header MATCHES "void MarkUsed\\(")
        target_compile_definitions(common PRIVATE EDEN_HOST_MEMORY_ZERO_TRACKING=1)
    endif()
    if(port_host_memory_header MATCHES "void HintCold\\(")
        target_compile_definitions(common PRIVATE EDEN_HOST_MEMORY_COLD_HINT=1)
    endif()

    # Folded BlockOfCode uses the allocator's distinct writable/executable views.
    target_include_directories(dynarmic BEFORE PUBLIC "${PORT_BUILD_DIR}/include")
    include("${EDEN_PORT_DIR}/jit-alias.cmake")

    target_sources(video_core PRIVATE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/port-support.cpp")
endfunction()

# jit-alias.cmake uses the same timestamp-preserving helper as the host checks.
if(NOT COMMAND write_derived)
    function(write_derived path content)
        file(WRITE "${path}.in" "${content}")
        configure_file("${path}.in" "${path}" COPYONLY)
    endfunction()
endif()
