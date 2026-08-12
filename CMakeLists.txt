cmake_minimum_required(VERSION 3.10)
project(lsfg-vk LANGUAGES CXX)
include(GNUInstallDirs)
include(CTest)

# === READ HERE FOR BUILD OPTIONS ===
option(LSFGVK_BUILD_VK_LAYER     "Build the Vulkan layer"            ON)
option(LSFGVK_BUILD_UI           "Build the user interface"         OFF)
option(LSFGVK_BUILD_CLI          "Build the command line interface"  ON)
option(LSFGVK_INSTALL_DEVELOP    "Install development libraries and headers"      OFF)
option(LSFGVK_INSTALL_XDG_FILES  "Install the application icon and desktop files" OFF)
set(LSFGVK_LAYER_LIBRARY_PATH liblsfg-vk-layer.so CACHE STRING "Change where Vulkan searches for the layer library")
option(LSFGVK_TESTING_RENDERDOC  "Enable RenderDoc integration for testing purposes" OFF)
# === READ HERE FOR BUILD OPTIONS ===

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_SKIP_RPATH ON)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(STATUS "Building with clang-specific diagnostics")

        set(CMAKE_CXX_CLANG_TIDY clang-tidy) # See .clang-tidy files

        add_compile_options(
            # By default, enable all warnings
            -Weverything
            # Some warnings are incompatible with each other
            -Wno-pre-c++20-compat-pedantic
            -Wno-c++98-compat-pedantic
            -Wno-switch-default
            # Then there's code-style things I don't care about
            -Wno-missing-designated-field-initializers
            -Wno-shadow
            # And functional warning I don't care about either
            -Wno-cast-function-type-strict
            -Wno-padded
        )
    endif()
endif()

if(LSFGVK_TESTING_RENDERDOC)
    add_compile_definitions(LSFGVK_TESTING_RENDERDOC)
endif()

if(LSFGVK_BUILD_VK_LAYER OR LSFGVK_BUILD_UI OR LSFGVK_BUILD_CLI OR
        LSFGVK_INSTALL_DEVELOP)
    add_subdirectory(lsfg-vk-common)
endif()
if(LSFGVK_BUILD_VK_LAYER OR LSFGVK_INSTALL_DEVELOP)
    add_subdirectory(lsfg-vk-backend)
endif()
if(LSFGVK_BUILD_VK_LAYER OR BUILD_TESTING)
    add_subdirectory(lsfg-vk-layer)
endif()
if(LSFGVK_BUILD_UI)
    add_subdirectory(lsfg-vk-ui)
endif()
if(LSFGVK_BUILD_CLI)
    add_subdirectory(lsfg-vk-cli)
endif()
