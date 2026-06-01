# =============================================================================
# DX12Engine — top-level CMakeLists
#
# Builds 4 targets:
#   * EngineCore  (StaticLibrary)  — engine code
#   * Editor      (Application)    — editor exe with ImGui (WITH_EDITOR)
#   * Game        (Application)    — runtime-only exe
#   * ShaderLab   (Application)    — shader-authoring sandbox (WITH_SHADERLAB)
#
# Quick start (Visual Studio 2022 + vcpkg):
#   cmake --preset vs-x64
#   cmake --build --preset vs-x64-release
#
# Quick start (manual):
#   set VCPKG_ROOT=C:\path\to\vcpkg
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
#         -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
#   cmake --build build --config Release
#
# Packaging:
#   cmake --build build --config Release --target package
#   → produces DX12Engine-<version>-win64.zip
# =============================================================================

cmake_minimum_required(VERSION 3.21)  # for $<TARGET_RUNTIME_DLLS>

# Pick up vcpkg toolchain automatically when VCPKG_ROOT is set, unless the user
# has already passed -DCMAKE_TOOLCHAIN_FILE.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

project(DX12Engine
    VERSION     1.1.0
    LANGUAGES   CXX C
    DESCRIPTION "Direct3D 12 deferred renderer + ECS + editor"
)

# Third-party libraries are vendored prebuilt under DX12/external/ (DirectXTex,
# Jolt, DXC, FreeType, Dear ImGui, Lua, sol2, nlohmann/json, meshoptimizer,
# Recast/Detour, FFmpeg, DirectX-Headers). Only Assimp comes from vcpkg — see
# vcpkg.json and DX12/CMakeLists.txt.

# -----------------------------------------------------------------------------
# Platform guard — Windows + MSVC + x64 only.
# -----------------------------------------------------------------------------
if(NOT WIN32)
    message(FATAL_ERROR "DX12Engine targets Windows only.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "DX12Engine requires MSVC (Visual Studio 2022 or newer).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "DX12Engine builds x64 only. Pass -A x64 to your VS generator.")
endif()

# -----------------------------------------------------------------------------
# Project options.
# -----------------------------------------------------------------------------
option(ENGINE_BUILD_EDITOR     "Build Editor.exe"    ON)
option(ENGINE_BUILD_GAME       "Build Game.exe"      ON)
option(ENGINE_BUILD_SHADERLAB  "Build ShaderLab.exe" ON)
option(ENGINE_PACKAGE_ASSETS   "Bundle asset/ into the CPack archive (multi-GB)" ON)

# -----------------------------------------------------------------------------
# Global compiler defaults — match the .vcxproj configuration exactly.
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# Output everything into <build>/bin/<config>/ so dlls + exes sit next to assets.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
foreach(cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/bin/${cfg}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/bin/${cfg}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/lib/${cfg}")
endforeach()

# Static MSVC runtime is the .vcxproj default; switch here if you ever need /MD.
# (Leave commented — defaults to dynamic /MD which matches the existing .sln.)
# set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

add_subdirectory(DX12)

# -----------------------------------------------------------------------------
# CPack — bundle binaries + runtime DLLs + shaders + (optionally) assets into
# DX12Engine-<version>-win64.zip.
# -----------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME           "DX12Engine")
set(CPACK_PACKAGE_VENDOR         "DX12Engine")
set(CPACK_PACKAGE_VERSION        "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME      "DX12Engine-${PROJECT_VERSION}-win64")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_GENERATOR              "ZIP")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "DX12Engine")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_VERBATIM_VARIABLES     ON)
include(CPack)
