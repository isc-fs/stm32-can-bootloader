cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME CAN_BL)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Bootloader-specific sources (CubeMX does not manage these).
    Core/Src/bl_proto.c
    Core/Src/bl_isotp.c
    Core/Src/bl_flash.c
    Core/Src/bl_fwinfo.c
    Core/Src/bl_health.c
    Core/Src/bl_dtc.c
    Core/Src/bl_fault.c
    Core/Src/bl_fdcan.c
    Core/Src/bl_log.c
    Core/Src/bl_live.c
    Core/Src/bl_node_id.c
    Core/Src/bl_nvm.c
    Core/Src/bl_provision.c
    Core/Src/bl_obyte.c
    Core/Src/bl_iwdg.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Core/Inc is already exported by the stm32cubemx interface library,
    # so bl_proto.h / bl_memmap.h / bl_config.h resolve without adding
    # anything here. Keep this block for future vendor / library paths.
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Per-board provisioning knob. Default of 0x1 is set in
    # Core/Inc/bl_config.h; override per-board at configure time, e.g.
    # `cmake -B build -DBL_NODE_ID=0x2`. Uncomment the line below to
    # pin a default at the build system level instead.
    # BL_NODE_ID=0x1
)

# If BL_NODE_ID was provided on the cmake command line, forward it as a
# preprocessor symbol so bl_config.h's #ifndef guard picks it up.
if(DEFINED BL_NODE_ID)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        BL_NODE_ID=${BL_NODE_ID}
    )
endif()

# Independent-watchdog knobs (#125 H6). Defaults live in bl_config.h; these
# forward command-line overrides, e.g. `cmake -B build -DBL_IWDG_ENABLE=0`
# to compile the watchdog out for bring-up, or -DBL_IWDG_RELOAD=1250 to
# lengthen the period after bench measurement.
if(DEFINED BL_IWDG_ENABLE)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        BL_IWDG_ENABLE=${BL_IWDG_ENABLE}
    )
endif()
if(DEFINED BL_IWDG_RELOAD)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        BL_IWDG_RELOAD=${BL_IWDG_RELOAD}
    )
endif()

# BL_FDCAN_INSTANCE (issue #120 Phase A). Selects which FDCAN peripheral
# hosts the bootloader bus; default in bl_config.h is 2 (FDCAN2).
if(DEFINED BL_FDCAN_INSTANCE)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        BL_FDCAN_INSTANCE=${BL_FDCAN_INSTANCE}
    )
endif()

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# Post-build: emit .hex and .bin next to the .elf and print section sizes.
# CubeIDE did this automatically; the CMake generator does not.
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
    COMMAND ${CMAKE_SIZE}              $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
    COMMENT "Generating .hex / .bin + section sizes for ${CMAKE_PROJECT_NAME}"
)
