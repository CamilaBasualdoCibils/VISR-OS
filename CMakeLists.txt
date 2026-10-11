cmake_minimum_required(VERSION 3.25)

project(
    VISROS
    VERSION 0.1.0
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VISROS_BUILD_DEVICE_EMULATOR "Build the VM/development XR device emulator" ON)
option(VISROS_BUILD_XR_DISPLAY_BRIDGE "Build the host OpenXR virtual-display bridge" ON)

include(GNUInstallDirs)

add_subdirectory(VISR)
add_subdirectory(src)
add_subdirectory(tools)

add_custom_target(
    build-base
    COMMAND "${CMAKE_COMMAND}" -E env "VISROS_VISR_SERVER=$<TARGET_FILE:VisrServer>" "VISROS_VISR_DESKTOP=$<TARGET_FILE:VisrDesktop>" "VISROS_VISR_LOGIN=$<TARGET_FILE:VisrLogin>" "VISROS_VISR_BOOT=$<TARGET_FILE:VisrBoot>" "${CMAKE_SOURCE_DIR}/scripts/build-base.sh"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Building the hardware-independent VISR OS Base"
    USES_TERMINAL
    VERBATIM
)

add_custom_target(
    build-sandbox
    COMMAND "${CMAKE_COMMAND}" -E env "VISROS_VISR_SERVER=$<TARGET_FILE:VisrServer>" "VISROS_VISR_DESKTOP=$<TARGET_FILE:VisrDesktop>" "VISROS_VISR_LOGIN=$<TARGET_FILE:VisrLogin>" "VISROS_VISR_BOOT=$<TARGET_FILE:VisrBoot>" "${CMAKE_SOURCE_DIR}/scripts/build-vm.sh"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Building the VISR OS NixOS VM"
    USES_TERMINAL
    VERBATIM
)

add_custom_target(
    run-sandbox
    COMMAND "${CMAKE_SOURCE_DIR}/scripts/run-sandbox.sh"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running the VISR OS Sandbox with QEMU/KVM"
    USES_TERMINAL
    VERBATIM
)

add_dependencies(build-base VisrServer VisrBoot VisrLogin VisrDesktop)
add_dependencies(build-sandbox build-base VisrServer VisrBoot VisrLogin VisrDesktop)
add_dependencies(run-sandbox build-sandbox)

if(VISROS_BUILD_XR_DISPLAY_BRIDGE)
add_custom_target(
    run-xr-display
    COMMAND "${CMAKE_SOURCE_DIR}/scripts/run-xr-display.sh"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running the VISR OS XR Display Bridge"
    USES_TERMINAL
    VERBATIM
)
add_dependencies(run-xr-display VISROSXRDisplayBridge)
endif()

# Keep the complete target graph readable in CMake-aware IDEs and CMake Tools.
# Assign after all subdirectories so targets created indirectly (tests, shaders,
# FrameGraph, and custom VM commands) are covered as well.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

function(visros_assign_target_folders directory)
    string(FIND "${directory}" "${CMAKE_SOURCE_DIR}/VISR" visr_prefix)
    if(visr_prefix EQUAL 0)
        set(folder "VISR")
    else()
        set(folder "VISR OS")
    endif()

    get_property(directory_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS directory_targets)
        set_property(TARGET "${target}" PROPERTY FOLDER "${folder}")
    endforeach()

    get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(subdirectory IN LISTS subdirectories)
        visros_assign_target_folders("${subdirectory}")
    endforeach()
endfunction()

visros_assign_target_folders("${CMAKE_SOURCE_DIR}")
