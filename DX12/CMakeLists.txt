# =============================================================================
# DX12/CMakeLists.txt — target definitions.
#
# Source lists are discovered with file(GLOB_RECURSE ... CONFIGURE_DEPENDS) so
# that adding/removing a .cpp under src/ is picked up on the next build without
# editing this file. The few files that must NOT land in EngineCore (app entry
# points, editor-only tooling, per-target scenes, and superseded duplicates) are
# filtered out explicitly and documented below.
#
# All paths are relative to DX12/, so they line up 1:1 with the source tree.
# The CMake invocation itself lives at the project root.
# =============================================================================

# -----------------------------------------------------------------------------
# Helper paths.
# -----------------------------------------------------------------------------
set(ENGINE_ROOT     "${CMAKE_CURRENT_SOURCE_DIR}")
set(ENGINE_INCLUDE  "${ENGINE_ROOT}/include")
set(ENGINE_SRC      "${ENGINE_ROOT}/src")
set(ENGINE_EXTERNAL "${ENGINE_ROOT}/external")
set(ENGINE_SHADERS  "${ENGINE_ROOT}/shaders")
set(ENGINE_ASSETS   "${ENGINE_ROOT}/asset")

# Note: the source tree contains a typo — "Resoruce" (not "Resource"). The
# directory on disk really is src/Resoruce/, while headers live in include/Resource/.
# Keep using src/Resoruce/ here so we match the tree exactly.

# -----------------------------------------------------------------------------
# Small helper for the config-dependent prebuilt static libs shipped in external/
# (separate Debug / Release .lib, with a single public include directory).
# -----------------------------------------------------------------------------
function(engine_import_static tgt dbg rel inc)
    add_library(${tgt} STATIC IMPORTED GLOBAL)
    set_target_properties(${tgt} PROPERTIES
        IMPORTED_LOCATION_DEBUG          "${dbg}"
        IMPORTED_LOCATION_RELEASE        "${rel}"
        IMPORTED_LOCATION_RELWITHDEBINFO "${rel}"
        IMPORTED_LOCATION_MINSIZEREL     "${rel}"
        INTERFACE_INCLUDE_DIRECTORIES    "${inc}"
    )
endfunction()

# =============================================================================
# Third-party imported targets.
#
# Everything is prebuilt under external/<lib>/ and shipped with the repo, except
# Assimp which comes from vcpkg.
# =============================================================================

# ----- Assimp (vcpkg) --------------------------------------------------------
find_package(assimp CONFIG REQUIRED)

# ----- DirectXTex (config-dependent prebuilt static lib) ---------------------
engine_import_static(dx12engine::DirectXTex
    "${ENGINE_EXTERNAL}/DirectXTex/lib/DirectXTex_Debug.lib"
    "${ENGINE_EXTERNAL}/DirectXTex/lib/DirectXTex_Release.lib"
    "${ENGINE_EXTERNAL}/DirectXTex")

# ----- Jolt (config-dependent prebuilt static lib + compile defs) ------------
add_library(dx12engine::Jolt STATIC IMPORTED GLOBAL)
set_target_properties(dx12engine::Jolt PROPERTIES
    IMPORTED_LOCATION_DEBUG          "${ENGINE_EXTERNAL}/Jolt/lib/Jolt_Debug.lib"
    IMPORTED_LOCATION_RELEASE        "${ENGINE_EXTERNAL}/Jolt/lib/Jolt_Release.lib"
    IMPORTED_LOCATION_RELWITHDEBINFO "${ENGINE_EXTERNAL}/Jolt/lib/Jolt_Release.lib"
    IMPORTED_LOCATION_MINSIZEREL     "${ENGINE_EXTERNAL}/Jolt/lib/Jolt_Release.lib"
    INTERFACE_INCLUDE_DIRECTORIES    "${ENGINE_EXTERNAL}"
    INTERFACE_COMPILE_DEFINITIONS    "JPH_DEBUG_RENDERER;JPH_PROFILE_ENABLED;JPH_OBJECT_STREAM;JPH_FLOATING_POINT_EXCEPTIONS_ENABLED"
)

# ----- DXC (single-config import lib + runtime DLLs) -------------------------
add_library(dx12engine::Dxc SHARED IMPORTED GLOBAL)
set_target_properties(dx12engine::Dxc PROPERTIES
    IMPORTED_IMPLIB               "${ENGINE_EXTERNAL}/dxc/lib/dxcompiler.lib"
    IMPORTED_LOCATION             "${ENGINE_EXTERNAL}/dxc/bin/dxcompiler.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${ENGINE_EXTERNAL}/dxc/include"
)
set(DXC_RUNTIME_DLLS
    "${ENGINE_EXTERNAL}/dxc/bin/dxcompiler.dll"
    "${ENGINE_EXTERNAL}/dxc/bin/dxil.dll"
)

# ----- FreeType (single-config prebuilt static lib) --------------------------
add_library(dx12engine::FreeType STATIC IMPORTED GLOBAL)
set_target_properties(dx12engine::FreeType PROPERTIES
    IMPORTED_LOCATION             "${ENGINE_EXTERNAL}/FreeType/lib/freetype.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${ENGINE_EXTERNAL}/FreeType/include"
)

# ----- meshoptimizer (config-dependent prebuilt static lib) ------------------
# Used by the collision-mesh baker (mesh simplify / optimize).
engine_import_static(dx12engine::meshoptimizer
    "${ENGINE_EXTERNAL}/meshoptimizer/Debug/meshoptimizer_debug.lib"
    "${ENGINE_EXTERNAL}/meshoptimizer/Release/meshoptimizer_release.lib"
    "${ENGINE_EXTERNAL}/meshoptimizer")

# ----- Recast / Detour navigation (config-dependent prebuilt static libs) ----
# Recast (navmesh bake) + Detour (query) + crowd + tile-cache.
engine_import_static(dx12engine::Recast
    "${ENGINE_EXTERNAL}/recastnavigation/Recast/Debug/Recast-d.lib"
    "${ENGINE_EXTERNAL}/recastnavigation/Recast/Release/Recast.lib"
    "${ENGINE_EXTERNAL}/recastnavigation/Recast/Include")
engine_import_static(dx12engine::Detour
    "${ENGINE_EXTERNAL}/recastnavigation/Detour/Debug/Detour-d.lib"
    "${ENGINE_EXTERNAL}/recastnavigation/Detour/Release/Detour.lib"
    "${ENGINE_EXTERNAL}/recastnavigation/Detour/Include")
engine_import_static(dx12engine::DetourCrowd
    "${ENGINE_EXTERNAL}/recastnavigation/DetourCrowd/Debug/DetourCrowd-d.lib"
    "${ENGINE_EXTERNAL}/recastnavigation/DetourCrowd/Release/DetourCrowd.lib"
    "${ENGINE_EXTERNAL}/recastnavigation/DetourCrowd/Include")
engine_import_static(dx12engine::DetourTileCache
    "${ENGINE_EXTERNAL}/recastnavigation/DetourTileCache/Debug/DetourTileCache-d.lib"
    "${ENGINE_EXTERNAL}/recastnavigation/DetourTileCache/Release/DetourTileCache.lib"
    "${ENGINE_EXTERNAL}/recastnavigation/DetourTileCache/Include")
add_library(dx12engine::Recastnavigation INTERFACE IMPORTED GLOBAL)
set_target_properties(dx12engine::Recastnavigation PROPERTIES
    INTERFACE_LINK_LIBRARIES "dx12engine::Recast;dx12engine::Detour;dx12engine::DetourCrowd;dx12engine::DetourTileCache"
)

# ----- FFmpeg (import libs + version-suffixed runtime DLLs) ------------------
# Video subsystem: hardware (D3D12VA) decode via libav* + scaling via swscale.
add_library(dx12engine::FFmpeg INTERFACE IMPORTED GLOBAL)
set_target_properties(dx12engine::FFmpeg PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${ENGINE_EXTERNAL}/ffmpeg/include"
    INTERFACE_LINK_LIBRARIES
        "${ENGINE_EXTERNAL}/ffmpeg/lib/avcodec.lib;${ENGINE_EXTERNAL}/ffmpeg/lib/avformat.lib;${ENGINE_EXTERNAL}/ffmpeg/lib/avutil.lib;${ENGINE_EXTERNAL}/ffmpeg/lib/swscale.lib"
)
# Runtime DLLs live next to the engine working dir (DX12/); copy them by the engine.
set(FFMPEG_RUNTIME_DLLS
    "${ENGINE_ROOT}/avcodec-62.dll"
    "${ENGINE_ROOT}/avformat-62.dll"
    "${ENGINE_ROOT}/avutil-60.dll"
    "${ENGINE_ROOT}/swresample-6.dll"   # transitive dependency of avcodec
    "${ENGINE_ROOT}/swscale-9.dll"
)

# ----- DirectX 12 (system libs + Agility-style headers under external/DirectX12) -----
add_library(dx12engine::DirectX12 INTERFACE IMPORTED GLOBAL)
set_target_properties(dx12engine::DirectX12 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${ENGINE_EXTERNAL}/DirectX12"
    INTERFACE_LINK_LIBRARIES      "d3d12;dxgi;d3dcompiler;dxguid"
)

# =============================================================================
# EngineCore.lib — engine static library.
#
# Discovered by globbing src/**/*.cpp, then filtering out the files that belong
# to a specific .exe target or are superseded duplicates.
# =============================================================================
file(GLOB_RECURSE ENGINECORE_SOURCES CONFIGURE_DEPENDS "${ENGINE_SRC}/*.cpp")

# (1) App entry points + PCH stub — compiled per-exe / auto-generated by CMake.
list(REMOVE_ITEM ENGINECORE_SOURCES
    "${ENGINE_SRC}/main.cpp"
    "${ENGINE_SRC}/App.cpp"
    "${ENGINE_SRC}/pch.cpp"
)
# (2) Editor / ShaderLab-only sources (need ImGui + WITH_EDITOR) — added per-exe.
list(REMOVE_ITEM ENGINECORE_SOURCES
    "${ENGINE_SRC}/Reflection/ReflectionEditor.cpp"
    "${ENGINE_SRC}/Scene/ShaderLabScene.cpp"   # ShaderLab.exe only
    "${ENGINE_SRC}/Scene/NPRRampEditor.cpp"    # ShaderLab.exe only
)
list(FILTER ENGINECORE_SOURCES EXCLUDE REGEX "/src/Editor/")                 # all editor windows
# Exception to the src/Editor/ exclude above: DebugDrawSystem is plain engine
# driver code (no ImGui, no WITH_EDITOR) that just happens to live under
# src/Editor/ for organisation. Its header comment says it must "live in
# EngineCore so every exe links it" — App::m_debugDraw is an always-present member
# and only the App call sites are WITH_EDITOR-gated, so Game.exe must link it too.
# Re-add it here; it is dropped from the editor-window glob below to avoid a
# duplicate-symbol link in Editor.exe / ShaderLab.exe.
list(APPEND ENGINECORE_SOURCES "${ENGINE_SRC}/Editor/DebugDrawSystem.cpp")
list(FILTER ENGINECORE_SOURCES EXCLUDE REGEX "/src/Resoruce/[^/]*Importer\\.cpp$")  # offline cookers (editor only)
# (3) Game.exe-only source.
list(REMOVE_ITEM ENGINECORE_SOURCES "${ENGINE_SRC}/Resoruce/SkeletonGrantTable.cpp")
# (4) Superseded duplicates — the old monolithic SSR passes were replaced by the
#     src/Graphics/SSR/ subsystem; compiling both would be a duplicate-symbol error.
list(REMOVE_ITEM ENGINECORE_SOURCES
    "${ENGINE_SRC}/RenderGraph/RenderPass/SSRPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/SSRDepthHierarchyPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/SceneColorPyramidPass.cpp"
)
# Lua amalgamation (compiled as C, no PCH).
list(APPEND ENGINECORE_SOURCES "${ENGINE_EXTERNAL}/lua/onelua.c")

add_library(EngineCore STATIC ${ENGINECORE_SOURCES})

target_include_directories(EngineCore PUBLIC
    "${ENGINE_INCLUDE}"
    "${ENGINE_EXTERNAL}"                 # nlohmann/json.hpp, DirectX12, Jolt root, ...
    "${ENGINE_EXTERNAL}/imgui"
    "${ENGINE_EXTERNAL}/lua"
    "${ENGINE_EXTERNAL}/sol2/include"
    "${ENGINE_EXTERNAL}/ffmpeg/include"
)
target_include_directories(EngineCore PRIVATE
    "${ENGINE_EXTERNAL}/FreeType/include"
)

target_compile_definitions(EngineCore PUBLIC
    UNICODE _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _LIB
    JPH_DEBUG_RENDERER
    JPH_PROFILE_ENABLED
    JPH_OBJECT_STREAM
    JPH_FLOATING_POINT_EXCEPTIONS_ENABLED
)

target_compile_options(EngineCore PUBLIC
    /utf-8
    /bigobj
    /MP            # parallel compile across cores
    /Zc:__cplusplus
    /permissive-   # ConformanceMode=true
)

# PCH — auto-generated by CMake from include/pch.h (mirrors src/pch.cpp).
target_precompile_headers(EngineCore PRIVATE "${ENGINE_INCLUDE}/pch.h")

# Lua's onelua.c is C and must NOT consume the C++ PCH.
set_source_files_properties("${ENGINE_EXTERNAL}/lua/onelua.c"
    PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON
        LANGUAGE C
)

target_link_libraries(EngineCore PUBLIC
    dx12engine::DirectX12
    dx12engine::DirectXTex
    dx12engine::Jolt
    dx12engine::Dxc
    dx12engine::FreeType
    dx12engine::meshoptimizer
    dx12engine::Recastnavigation
    dx12engine::FFmpeg
    assimp::assimp
    xaudio2            # XAudio2 / X3DAudio (Windows SDK)
)

# -----------------------------------------------------------------------------
# Linker search paths for #pragma comment(lib, "...") directives.
#
# A lot of the engine declares its own link deps inline via bare-name pragmas
# (DirectXTex_Release.lib, avformat.lib, Recast.lib, meshoptimizer_release.lib,
# assimp-vc143-mt.lib, ...). Those /DEFAULTLIB directives only resolve if the
# containing folder is on the linker search path, so mirror the .vcxproj's
# AdditionalLibraryDirectories here. System pragmas (d3d11, Shcore, shell32,
# comdlg32, xaudio2, d3d12, ...) resolve from the Windows SDK path automatically.
# PUBLIC so the dirs propagate to the Editor / Game / ShaderLab link lines too
# (the editor-only importers pragma-link assimp).
# -----------------------------------------------------------------------------
target_link_directories(EngineCore PUBLIC
    "${ENGINE_EXTERNAL}/Jolt/lib"
    "${ENGINE_EXTERNAL}/dxc/lib"
    "${ENGINE_EXTERNAL}/FreeType/lib"
    "${ENGINE_EXTERNAL}/DirectXTex/lib"
    "${ENGINE_EXTERNAL}/ffmpeg/lib"
    "${ENGINE_EXTERNAL}/meshoptimizer/Debug"
    "${ENGINE_EXTERNAL}/meshoptimizer/Release"
    "${ENGINE_EXTERNAL}/recastnavigation/Recast/Debug"
    "${ENGINE_EXTERNAL}/recastnavigation/Recast/Release"
    "${ENGINE_EXTERNAL}/recastnavigation/Detour/Debug"
    "${ENGINE_EXTERNAL}/recastnavigation/Detour/Release"
    "${ENGINE_EXTERNAL}/recastnavigation/DetourCrowd/Debug"
    "${ENGINE_EXTERNAL}/recastnavigation/DetourCrowd/Release"
    "${ENGINE_EXTERNAL}/recastnavigation/DetourTileCache/Debug"
    "${ENGINE_EXTERNAL}/recastnavigation/DetourTileCache/Release"
)

# Assimp comes from vcpkg, so add its installed lib dir(s) to the search path as
# well (the editor importers pragma-link assimp by bare name).
get_target_property(_assimp_cfgs assimp::assimp IMPORTED_CONFIGURATIONS)
if(_assimp_cfgs)
    foreach(_cfg IN LISTS _assimp_cfgs)
        get_target_property(_assimp_loc assimp::assimp IMPORTED_LOCATION_${_cfg})
        if(_assimp_loc)
            get_filename_component(_assimp_dir "${_assimp_loc}" DIRECTORY)
            target_link_directories(EngineCore PUBLIC "${_assimp_dir}")
        endif()
    endforeach()
endif()

# Group sources nicely in the VS solution generated by CMake.
source_group(TREE "${ENGINE_ROOT}" PREFIX "Source Files" FILES ${ENGINECORE_SOURCES})

# =============================================================================
# Editor / Game / ShaderLab — shared source helpers.
# =============================================================================
set(IMGUI_SOURCES
    "${ENGINE_EXTERNAL}/imgui/imgui.cpp"
    "${ENGINE_EXTERNAL}/imgui/imgui_demo.cpp"
    "${ENGINE_EXTERNAL}/imgui/imgui_draw.cpp"
    "${ENGINE_EXTERNAL}/imgui/imgui_impl_dx12.cpp"
    "${ENGINE_EXTERNAL}/imgui/imgui_impl_win32.cpp"
    "${ENGINE_EXTERNAL}/imgui/imgui_tables.cpp"
    "${ENGINE_EXTERNAL}/imgui/imgui_widgets.cpp"
    "${ENGINE_EXTERNAL}/imgui/ImGuizmo/ImGuizmo.cpp"
)

# Sources that Editor and ShaderLab both need: editor windows, the reflection
# inspector, the offline asset importers, and imgui. Globbed so new editor
# windows / importers are picked up automatically.
file(GLOB EDITOR_WINDOW_SOURCES CONFIGURE_DEPENDS "${ENGINE_SRC}/Editor/*.cpp")
# DebugDrawSystem.cpp is compiled into EngineCore (see the exception above), so drop
# it from the editor-window glob — otherwise Editor.exe / ShaderLab.exe would define
# its symbols twice (once via EngineCore.lib, once here) and fail to link (LNK2005).
list(REMOVE_ITEM EDITOR_WINDOW_SOURCES "${ENGINE_SRC}/Editor/DebugDrawSystem.cpp")
file(GLOB IMPORTER_SOURCES      CONFIGURE_DEPENDS "${ENGINE_SRC}/Resoruce/*Importer.cpp")
set(EDITOR_SHARED_SOURCES
    "${ENGINE_SRC}/main.cpp"
    "${ENGINE_SRC}/App.cpp"
    "${ENGINE_SRC}/Reflection/ReflectionEditor.cpp"
    ${EDITOR_WINDOW_SOURCES}
    ${IMPORTER_SOURCES}
    ${IMGUI_SOURCES}
)

# Reusable setup shared by all 3 .exe targets.
function(engine_configure_app target_name)
    # Unicode + Win32 entry point (matches <SubSystem>Windows</SubSystem>).
    set_target_properties(${target_name} PROPERTIES WIN32_EXECUTABLE TRUE)

    target_link_libraries(${target_name} PRIVATE EngineCore)

    target_compile_options(${target_name} PRIVATE /utf-8 /bigobj /MP /permissive-)
    target_compile_definitions(${target_name} PRIVATE _CONSOLE)

    # PCH — each .exe compiles its own pch since defines differ.
    target_precompile_headers(${target_name} PRIVATE "${ENGINE_INCLUDE}/pch.h")

    # Copy DXC runtime DLLs (engine compiles HLSL via DXC at runtime).
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${DXC_RUNTIME_DLLS}
                $<TARGET_FILE_DIR:${target_name}>
        COMMAND_EXPAND_LISTS VERBATIM)

    # Copy FFmpeg runtime DLLs (video subsystem).
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${FFMPEG_RUNTIME_DLLS}
                $<TARGET_FILE_DIR:${target_name}>
        COMMAND_EXPAND_LISTS VERBATIM)

    # Copy Assimp + any other linked-lib runtime DLLs (vcpkg builds dynamic Assimp).
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${target_name}>
                $<TARGET_FILE_DIR:${target_name}>
        COMMAND_EXPAND_LISTS VERBATIM)

    # When launching from Visual Studio, run from DX12/ so the engine finds
    # shaders/ and asset/ relative to cwd.
    set_target_properties(${target_name} PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${ENGINE_ROOT}")
endfunction()

# imgui sources keep their own conventions — never inherit the engine PCH.
foreach(src IN LISTS IMGUI_SOURCES)
    set_source_files_properties(${src} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endforeach()

# -----------------------------------------------------------------------------
# Editor.exe
# -----------------------------------------------------------------------------
if(ENGINE_BUILD_EDITOR)
    add_executable(Editor ${EDITOR_SHARED_SOURCES})
    engine_configure_app(Editor)
    target_compile_definitions(Editor PRIVATE WITH_EDITOR)
    set_source_files_properties(${IMGUI_SOURCES}
        TARGET_DIRECTORY Editor
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

# -----------------------------------------------------------------------------
# Game.exe — runtime only.
# -----------------------------------------------------------------------------
if(ENGINE_BUILD_GAME)
    add_executable(Game
        "${ENGINE_SRC}/main.cpp"
        "${ENGINE_SRC}/App.cpp"
        "${ENGINE_SRC}/Resoruce/SkeletonGrantTable.cpp"
    )
    engine_configure_app(Game)
endif()

# -----------------------------------------------------------------------------
# ShaderLab.exe — editor surface + extra ShaderLab scenes.
# -----------------------------------------------------------------------------
if(ENGINE_BUILD_SHADERLAB)
    add_executable(ShaderLab
        ${EDITOR_SHARED_SOURCES}
        "${ENGINE_SRC}/Scene/ShaderLabScene.cpp"
        "${ENGINE_SRC}/Scene/NPRRampEditor.cpp"
    )
    engine_configure_app(ShaderLab)
    target_compile_definitions(ShaderLab PRIVATE WITH_EDITOR WITH_SHADERLAB)
    set_source_files_properties(${IMGUI_SOURCES}
        TARGET_DIRECTORY ShaderLab
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

# Default startup project when CMake generates a .sln.
if(TARGET Editor)
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT Editor)
endif()

# =============================================================================
# Install rules — drive CPack ZIP.
# =============================================================================
include(GNUInstallDirs)

foreach(app IN ITEMS Editor Game ShaderLab)
    if(TARGET ${app})
        install(TARGETS ${app} RUNTIME DESTINATION ".")
        install(FILES $<TARGET_RUNTIME_DLLS:${app}>
                DESTINATION "." COMPONENT Runtime)
    endif()
endforeach()

# DXC + FFmpeg runtime — bundle explicitly (loaded via IMPORTED SHARED / plain files).
install(FILES ${DXC_RUNTIME_DLLS}    DESTINATION ".")
install(FILES ${FFMPEG_RUNTIME_DLLS} DESTINATION ".")

# Shaders — runtime DXC compile expects them under cwd/shaders/.
install(DIRECTORY "${ENGINE_SHADERS}/"
        DESTINATION "shaders"
        FILES_MATCHING
            PATTERN "*.hlsl"
            PATTERN "*.hlsli"
            PATTERN "shader_cache_dxil" EXCLUDE
)

# Lua scripts + AI behavior trees.
install(DIRECTORY "${ENGINE_ASSETS}/scripts" DESTINATION "asset")
install(DIRECTORY "${ENGINE_ASSETS}/ai"      DESTINATION "asset")

# Assets — large; gated by option so the ZIP can stay small for source-only releases.
if(ENGINE_PACKAGE_ASSETS)
    install(DIRECTORY "${ENGINE_ASSETS}/"
            DESTINATION "asset"
            PATTERN "scenes" EXCLUDE)     # editor-temporary
endif()
