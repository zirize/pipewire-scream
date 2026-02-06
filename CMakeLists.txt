cmake_minimum_required(VERSION 3.10)
project(pipewire-scream VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find PipeWire
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3>=0.3.0)
pkg_check_modules(SPA REQUIRED libspa-0.2)

# Module source files
add_library(pipewire-module-scream MODULE
    module-scream.c
)

# Include directories
target_include_directories(pipewire-module-scream PRIVATE
    ${PIPEWIRE_INCLUDE_DIRS}
    ${SPA_INCLUDE_DIRS}
)

# Link directories
target_link_directories(pipewire-module-scream PRIVATE
    ${PIPEWIRE_LIBRARY_DIRS}
)

# Link libraries
target_link_libraries(pipewire-module-scream
    ${PIPEWIRE_LIBRARIES}
)

# Compiler flags
target_compile_options(pipewire-module-scream PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -fvisibility=hidden
    ${PIPEWIRE_CFLAGS_OTHER}
)

# Set module properties
set_target_properties(pipewire-module-scream PROPERTIES
    PREFIX "libpipewire-"
    OUTPUT_NAME "module-scream"
    C_VISIBILITY_PRESET hidden
)

# Installation
if(NOT DEFINED PIPEWIRE_MODULE_DIR)
    pkg_get_variable(PIPEWIRE_MODULE_DIR libpipewire-0.3 moduledir)
endif()

if(NOT PIPEWIRE_MODULE_DIR)
    message(WARNING "Could not determine PipeWire module directory, using default")
    set(PIPEWIRE_MODULE_DIR "${CMAKE_INSTALL_PREFIX}/lib/pipewire-0.3")
endif()

message(STATUS "PipeWire module directory: ${PIPEWIRE_MODULE_DIR}")

install(TARGETS pipewire-module-scream
    LIBRARY DESTINATION ${PIPEWIRE_MODULE_DIR}
)

# Print build information
message(STATUS "")
message(STATUS "Scream PipeWire Sender Module Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  PipeWire version: ${PIPEWIRE_VERSION}")
message(STATUS "  Install path: ${PIPEWIRE_MODULE_DIR}")
message(STATUS "")
