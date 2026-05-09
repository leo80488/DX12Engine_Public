# =============================================================================
# DX12/CMakeLists.txt — actual target definitions.
#
# All paths in this file are relative to DX12/, so they line up 1:1 with the
# existing .vcxproj source lists. CMake invocation lives at the project root.
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
# Keep using src/Resoruce/ here so we match the .vcxproj exactly.

# =============================================================================
# Third-party imported targets.
#
# All libs are prebuilt in external/<lib>/lib/ and shipped with the repo, except
# Assimp which comes from vcpkg.
# =============================================================================

# ----- Assimp (vcpkg) --------------------------------------------------------
find_package(assimp CONFIG REQUIRED)

# ----- DirectXTex (configuration-dependent prebuilt static lib) --------------
add_library(dx12engine::DirectXTex STATIC IMPORTED GLOBAL)
set_target_properties(dx12engine::DirectXTex PROPERTIES
    IMPORTED_LOCATION_DEBUG          "${ENGINE_EXTERNAL}/DirectXTex/lib/DirectXTex_Debug.lib"
    IMPORTED_LOCATION_RELEASE        "${ENGINE_EXTERNAL}/DirectXTex/lib/DirectXTex_Release.lib"
    IMPORTED_LOCATION_RELWITHDEBINFO "${ENGINE_EXTERNAL}/DirectXTex/lib/DirectXTex_Release.lib"
    IMPORTED_LOCATION_MINSIZEREL     "${ENGINE_EXTERNAL}/DirectXTex/lib/DirectXTex_Release.lib"
    INTERFACE_INCLUDE_DIRECTORIES    "${ENGINE_EXTERNAL}/DirectXTex"
)

# ----- Jolt (configuration-dependent prebuilt static lib) --------------------
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

# ----- DirectX 12 (system libs + Agility-style headers under external/DirectX12) -----
add_library(dx12engine::DirectX12 INTERFACE IMPORTED GLOBAL)
set_target_properties(dx12engine::DirectX12 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${ENGINE_EXTERNAL}/DirectX12"
    INTERFACE_LINK_LIBRARIES      "d3d12;dxgi;d3dcompiler;dxguid"
)

# =============================================================================
# EngineCore.lib — engine static library.
# =============================================================================
set(ENGINECORE_SOURCES
    # PCH (CMake auto-generates the create-side; we only list user .cpp here)

    # Top-level
    "${ENGINE_SRC}/SceneSerialization.cpp"

    # AI
    "${ENGINE_SRC}/AI/ActionRegistry.cpp"
    "${ENGINE_SRC}/AI/AILODSystem.cpp"
    "${ENGINE_SRC}/AI/AISystem.cpp"
    "${ENGINE_SRC}/AI/BTAsset.cpp"
    "${ENGINE_SRC}/AI/BTNodes.cpp"
    "${ENGINE_SRC}/AI/LuaBTBindings.cpp"

    # Audio
    "${ENGINE_SRC}/Audio/Audio3DSystem.cpp"
    "${ENGINE_SRC}/Audio/AudioClipLoader.cpp"
    "${ENGINE_SRC}/Audio/AudioClipSystem.cpp"
    "${ENGINE_SRC}/Audio/AudioEngine.cpp"
    "${ENGINE_SRC}/Audio/AudioImporter.cpp"
    "${ENGINE_SRC}/Audio/AudioSystem.cpp"

    # ECS
    "${ENGINE_SRC}/ECS/AnimationSystem.cpp"
    "${ENGINE_SRC}/ECS/CameraSystem.cpp"
    "${ENGINE_SRC}/ECS/CommandAPI.cpp"
    "${ENGINE_SRC}/ECS/Components.cpp"
    "${ENGINE_SRC}/ECS/DecalSpawner.cpp"
    "${ENGINE_SRC}/ECS/DecalSystem.cpp"
    "${ENGINE_SRC}/ECS/ECS.cpp"
    "${ENGINE_SRC}/ECS/FollowSystem.cpp"
    "${ENGINE_SRC}/ECS/IKSystem.cpp"
    "${ENGINE_SRC}/ECS/MaterialReflectionSync.cpp"
    "${ENGINE_SRC}/ECS/SocketSystem.cpp"

    # Graphics
    "${ENGINE_SRC}/Graphics/BeamSystem.cpp"
    "${ENGINE_SRC}/Graphics/DDGISceneAS.cpp"
    "${ENGINE_SRC}/Graphics/DDGIVolumeManager.cpp"
    "${ENGINE_SRC}/Graphics/DescriptorHeapAllocator.cpp"
    "${ENGINE_SRC}/Graphics/DxcCompiler.cpp"
    "${ENGINE_SRC}/Graphics/GPUProfiler.cpp"
    "${ENGINE_SRC}/Graphics/Graphics.cpp"
    "${ENGINE_SRC}/Graphics/GraphicsDX12.cpp"
    "${ENGINE_SRC}/Graphics/MaterialCBVRing.cpp"
    "${ENGINE_SRC}/Graphics/MaterialSRVRing.cpp"
    "${ENGINE_SRC}/Graphics/MeshDescriptorHeap.cpp"
    "${ENGINE_SRC}/Graphics/MeshManager.cpp"
    "${ENGINE_SRC}/Graphics/ParticleSystem.cpp"
    "${ENGINE_SRC}/Graphics/PSOCache.cpp"
    "${ENGINE_SRC}/Graphics/Raytracing.cpp"
    "${ENGINE_SRC}/Graphics/ReflectionProbeManager.cpp"
    "${ENGINE_SRC}/Graphics/Renderer.cpp"
    "${ENGINE_SRC}/Graphics/RHICommandList.cpp"
    "${ENGINE_SRC}/Graphics/SceneBVH.cpp"
    "${ENGINE_SRC}/Graphics/ShaderLibrary.cpp"
    "${ENGINE_SRC}/Graphics/ShaderReflection.cpp"
    "${ENGINE_SRC}/Graphics/ShadingModel.cpp"
    "${ENGINE_SRC}/Graphics/ShadowFrustumCompute.cpp"
    "${ENGINE_SRC}/Graphics/ShadowSystem.cpp"
    "${ENGINE_SRC}/Graphics/SkinnedMeshSubsystem.cpp"
    "${ENGINE_SRC}/Graphics/SkinningBuffers.cpp"
    "${ENGINE_SRC}/Graphics/TracerSystem.cpp"
    "${ENGINE_SRC}/Graphics/TrailSystem.cpp"

    # Physics
    "${ENGINE_SRC}/Physics/ChainPhysicsSystem.cpp"
    "${ENGINE_SRC}/Physics/PhysicsSystem.cpp"

    # PostProcess
    "${ENGINE_SRC}/PostProcess/BuiltinPostProcessEffects.cpp"
    "${ENGINE_SRC}/PostProcess/EntityVolumeSource.cpp"
    "${ENGINE_SRC}/PostProcess/ParameterBlender.cpp"
    "${ENGINE_SRC}/PostProcess/PostProcessStack.cpp"
    "${ENGINE_SRC}/PostProcess/ScriptedOverride.cpp"
    "${ENGINE_SRC}/PostProcess/VolumeSystem.cpp"

    # RenderGraph + RenderPass (27 passes)
    "${ENGINE_SRC}/RenderGraph/RenderContext.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderGraph.cpp"
    "${ENGINE_SRC}/RenderGraph/TestTrianglePass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/AutoExposurePass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/BeamSimPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/BloomPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/CASPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/ClusterPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/CullingPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/DDGIPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/DDGIProbeDebugPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/DebugWirePass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/DecalPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/GBufferPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/GlassShatterPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/HiZPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/LensFlarePass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/LightingPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/OutlinePass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/ParticlePasses.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/PickingPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/ReflectionProbeCapturePass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/SceneColorPyramidPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/SceneVoxelPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/ShadowPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/SkinningPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/SkyboxPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/SkyIBLPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/SpotShadowPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/SSRDepthHierarchyPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/SSRPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/TAAPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/TerrainPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/ToneMapPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/TracerPasses.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/TrailPasses.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/TransparentPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/UIPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/VolumetricFogPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/WorldUIBillboardPass.cpp"
    "${ENGINE_SRC}/RenderGraph/RenderPass/XeGTAOPass.cpp"

    # Resoruce  [sic — directory name on disk really is "Resoruce"]
    "${ENGINE_SRC}/Resoruce/AnimationClipSystem.cpp"
    "${ENGINE_SRC}/Resoruce/AnimationLoader.cpp"
    "${ENGINE_SRC}/Resoruce/AssetFS.cpp"
    "${ENGINE_SRC}/Resoruce/AssetManager.cpp"
    "${ENGINE_SRC}/Resoruce/BCCompressor.cpp"
    "${ENGINE_SRC}/Resoruce/ComponentSerializers.cpp"
    "${ENGINE_SRC}/Resoruce/DDSTextureLoader.cpp"
    "${ENGINE_SRC}/Resoruce/DecalMaterialAsset.cpp"
    "${ENGINE_SRC}/Resoruce/DecalMaterialLibrary.cpp"
    "${ENGINE_SRC}/Resoruce/DecalMaterialSerializer.cpp"
    "${ENGINE_SRC}/Resoruce/MaterialLoader.cpp"
    "${ENGINE_SRC}/Resoruce/MaterialSerializer.cpp"
    "${ENGINE_SRC}/Resoruce/MaterialSystem.cpp"
    "${ENGINE_SRC}/Resoruce/MeshLibrary.cpp"
    "${ENGINE_SRC}/Resoruce/MeshSystem.cpp"
    "${ENGINE_SRC}/Resoruce/PostProcessConfig.cpp"
    "${ENGINE_SRC}/Resoruce/PrefabSerializer.cpp"
    "${ENGINE_SRC}/Resoruce/ProceduralMesh.cpp"
    "${ENGINE_SRC}/Resoruce/ResourceManager.cpp"
    "${ENGINE_SRC}/Resoruce/ShaderLoader.cpp"
    "${ENGINE_SRC}/Resoruce/TextureLoader.cpp"
    "${ENGINE_SRC}/Resoruce/TextureSystem.cpp"
    "${ENGINE_SRC}/Resoruce/WorldSerializer.cpp"

    # Scene
    "${ENGINE_SRC}/Scene/EndScene.cpp"
    "${ENGINE_SRC}/Scene/GameScene.cpp"
    "${ENGINE_SRC}/Scene/MeshSpawner.cpp"
    "${ENGINE_SRC}/Scene/Ray.cpp"
    "${ENGINE_SRC}/Scene/SceneInstanceLoader.cpp"
    "${ENGINE_SRC}/Scene/SceneManager.cpp"
    "${ENGINE_SRC}/Scene/TestScene.cpp"
    "${ENGINE_SRC}/Scene/TitleScene.cpp"
    "${ENGINE_SRC}/Scene/TransformSystem.cpp"

    # Scripting
    "${ENGINE_SRC}/Scripting/ScriptSystem.cpp"

    # System
    "${ENGINE_SRC}/System/Exception_handle.cpp"
    "${ENGINE_SRC}/System/FileWatcher.cpp"
    "${ENGINE_SRC}/System/Keyboard.cpp"
    "${ENGINE_SRC}/System/Log.cpp"
    "${ENGINE_SRC}/System/Mouse.cpp"
    "${ENGINE_SRC}/System/TaskSystem.cpp"
    "${ENGINE_SRC}/System/Timer.cpp"
    "${ENGINE_SRC}/System/Window.cpp"

    # UI
    "${ENGINE_SRC}/UI/BasicWidgets.cpp"
    "${ENGINE_SRC}/UI/Font.cpp"
    "${ENGINE_SRC}/UI/LuaUIBindings.cpp"
    "${ENGINE_SRC}/UI/Tween.cpp"
    "${ENGINE_SRC}/UI/UIDrawList.cpp"
    "${ENGINE_SRC}/UI/UISystem.cpp"
    "${ENGINE_SRC}/UI/Widget.cpp"
    "${ENGINE_SRC}/UI/WidgetRegistry.cpp"
    "${ENGINE_SRC}/UI/WorldSpaceUISystem.cpp"

    # Lua (compiled as C, no PCH)
    "${ENGINE_EXTERNAL}/lua/onelua.c"
)

add_library(EngineCore STATIC ${ENGINECORE_SOURCES})

target_include_directories(EngineCore PUBLIC
    "${ENGINE_INCLUDE}"
    "${ENGINE_EXTERNAL}"
    "${ENGINE_EXTERNAL}/imgui"
    "${ENGINE_EXTERNAL}/lua"
    "${ENGINE_EXTERNAL}/sol2/include"
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

# PCH — auto-generated by CMake from include/pch.h, mirrors the .vcxproj.
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
    assimp::assimp
    # XAudio2 / X3DAudio (Windows SDK)
    xaudio2
)

# Group sources nicely in the VS solution generated by CMake.
source_group(TREE "${ENGINE_ROOT}" PREFIX "Source Files" FILES ${ENGINECORE_SOURCES})

# =============================================================================
# Editor / Game / ShaderLab — shared editor source list helper.
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

# Sources that Editor and ShaderLab both need (importers, EditorLayer, imgui).
set(EDITOR_SHARED_SOURCES
    "${ENGINE_SRC}/main.cpp"
    "${ENGINE_SRC}/App.cpp"
    "${ENGINE_SRC}/Editor/AnimationDebugWindow.cpp"
    "${ENGINE_SRC}/Editor/EditorLayer.cpp"
    "${ENGINE_SRC}/Editor/FontEditorWindow.cpp"
    "${ENGINE_SRC}/Editor/ImGuiManager.cpp"
    "${ENGINE_SRC}/Editor/SSRDebugWindow.cpp"
    "${ENGINE_SRC}/Editor/TimelineEditor.cpp"
    "${ENGINE_SRC}/Reflection/ReflectionEditor.cpp"
    "${ENGINE_SRC}/Resoruce/AnimationImporter.cpp"
    "${ENGINE_SRC}/Resoruce/MaterialImporter.cpp"
    "${ENGINE_SRC}/Resoruce/MeshImporter.cpp"
    "${ENGINE_SRC}/Resoruce/PmxImporter.cpp"
    "${ENGINE_SRC}/Resoruce/SceneImporter.cpp"
    "${ENGINE_SRC}/Resoruce/ShaderImporter.cpp"
    "${ENGINE_SRC}/Resoruce/SkeletonImporter.cpp"
    "${ENGINE_SRC}/Resoruce/TextureImporter.cpp"
    "${ENGINE_SRC}/Resoruce/VmdImporter.cpp"
    "${ENGINE_SRC}/Resoruce/VrmImporter.cpp"
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

    # Copy DXC runtime DLLs next to the exe (engine compiles HLSL via DXC at runtime).
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${DXC_RUNTIME_DLLS}
                $<TARGET_FILE_DIR:${target_name}>
        COMMAND_EXPAND_LISTS
        VERBATIM
    )

    # Copy Assimp + any other linked-lib runtime DLLs (vcpkg builds dynamic Assimp).
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${target_name}>
                $<TARGET_FILE_DIR:${target_name}>
        COMMAND_EXPAND_LISTS
        VERBATIM
    )

    # When launching from Visual Studio, run from DX12/ so the engine finds
    # shaders/ and asset/ relative to cwd (matches the .vcxproj convention).
    set_target_properties(${target_name} PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${ENGINE_ROOT}"
    )
endfunction()

# Sources brought in by editor-mode builds must NOT inherit the EngineCore PCH
# (imgui has its own conventions; CMake's per-target PCH already excludes them
# via SKIP_PRECOMPILE_HEADERS below).
function(engine_skip_pch_for_imgui)
    foreach(src IN LISTS IMGUI_SOURCES)
        set_source_files_properties(${src} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    endforeach()
endfunction()

# -----------------------------------------------------------------------------
# Editor.exe
# -----------------------------------------------------------------------------
if(ENGINE_BUILD_EDITOR)
    add_executable(Editor ${EDITOR_SHARED_SOURCES})
    engine_configure_app(Editor)
    target_compile_definitions(Editor PRIVATE WITH_EDITOR)
    set_source_files_properties(${IMGUI_SOURCES}
        TARGET_DIRECTORY Editor
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON
    )
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
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON
    )
endif()

# Default startup project when CMake generates a .sln.
if(TARGET Editor)
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT Editor)
endif()

# =============================================================================
# Install rules — drive CPack ZIP.
# =============================================================================
include(GNUInstallDirs)

# Binaries → install/bin/<exe> (top-level ZIP root via CPACK_INCLUDE_TOPLEVEL_DIRECTORY).
foreach(app IN ITEMS Editor Game ShaderLab)
    if(TARGET ${app})
        install(TARGETS ${app} RUNTIME DESTINATION ".")
        # Also install dependent runtime DLLs (DXC, Assimp, ...).
        install(FILES $<TARGET_RUNTIME_DLLS:${app}>
                DESTINATION "."
                COMPONENT Runtime)
    endif()
endforeach()

# DXC runtime — bundle dxcompiler.dll + dxil.dll explicitly (DXC is loaded via
# IMPORTED SHARED but $<TARGET_RUNTIME_DLLS> needs CMake to know the right
# IMPORTED_LOCATION; pasting the files directly is unambiguous).
install(FILES ${DXC_RUNTIME_DLLS} DESTINATION ".")

# Shaders — runtime DXC compile expects them under cwd/shaders/.
install(DIRECTORY "${ENGINE_SHADERS}/"
        DESTINATION "shaders"
        FILES_MATCHING
            PATTERN "*.hlsl"
            PATTERN "*.hlsli"
            PATTERN "shader_cache_dxil" EXCLUDE
)

# Lua scripts.
install(DIRECTORY "${ENGINE_ASSETS}/scripts" DESTINATION "asset")
install(DIRECTORY "${ENGINE_ASSETS}/ai"      DESTINATION "asset")

# Assets — large; gated by option so the ZIP can stay small for CI / source-only releases.
if(ENGINE_PACKAGE_ASSETS)
    install(DIRECTORY "${ENGINE_ASSETS}/"
            DESTINATION "asset"
            PATTERN "scenes" EXCLUDE     # editor-temporary
    )
endif()
