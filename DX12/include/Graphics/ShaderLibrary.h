#pragma once

// ShaderLibrary — manages compiled shader bytecode and GPU shader handles.
//
// Dev-mode flow (ENGINE_DEV, always enabled in this codebase):
//   1. For each (ShaderID, PermutationKey): derive cache path (.ishdr on disk).
//   2. If cache exists: read the DXIL container from the .ishdr blob (no recompile).
//   3. If not: compile .hlsl through DxcCompiler (SM 6.6, HV 2018) with
//              permutation #defines; write the resulting .ishdr to disk.
//   4. Call IGraphicsDevice::CreateShader() -> RHI::Shader GPU handle.
//   5. Cache handle in memory.
//
// Per-frame usage: GetShader() is an O(1) hashmap lookup — zero compilation.
//
// No DX12 types (ID3D12*, DXGI_FORMAT) appear in this header.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/PermutationKey.h"
#include "Graphics/ShaderReflection.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

class IGraphicsDevice;

// Identifies a named shader within the engine's library.
// Maps to a specific .hlsl file + stage + entry point.
enum class ShaderID : uint32_t
{
    GBuffer_VS          = 0,
    GBuffer_PS          = 1,
    Lighting_VS         = 2,
    Lighting_PS         = 3,
    PickingID_VS        = 4,
    PickingID_PS        = 5,
    Skybox_VS           = 6,
    Skybox_PS           = 7,
    BloomDownsample_CS  = 8,
    BloomUpsample_CS    = 9,
    HistogramBuild_CS   = 10,
    HistogramAverage_CS = 11,
    ToneMap_CS          = 12,
    Transparent_PS      = 13,
    Shadow_VS              = 14,
    Shadow_PS              = 15,
    TAA_CS                 = 16,
    OutlineHull_VS         = 17,
    OutlineHull_PS         = 18,
    OutlineObjectID_PS     = 19,
    OutlineScreenSpace_PS  = 20,
    SkinCS                 = 21,  // Compute skinning: restPose + blendData + poseMatrices -> skinnedVertices
    ChainIntegrate_CS      = 22,  // Chain physics: Verlet integration
    ChainConstraint_CS     = 23,  // Chain physics: PBD distance constraint
    ChainWriteBone_CS      = 24,  // Chain physics: particle positions -> bone matrices
    ClusterBuild_CS        = 25,  // Clustered lighting: build cluster AABBs
    ClusterCull_CS         = 26,  // Clustered lighting: assign lights to clusters
    Billboard_VS           = 27,  // Light icon billboard vertex shader
    Billboard_PS           = 28,  // Light icon billboard pixel shader
    GenerateLUT_CS         = 29,  // Color grading: bake procedural 3D LUT
    InstanceCull_CS        = 30,  // GPU frustum culling compute shader
    HiZGenerate_CS         = 31,  // Hi-Z mip chain generation
    DebugWire_VS           = 32,  // Debug wireframe vertex shader
    DebugWire_PS           = 33,  // Debug wireframe pixel shader
    XeGTAO_CS              = 34,  // XeGTAO main AO compute shader
    XeGTAODenoise_CS       = 35,  // XeGTAO spatial denoise compute shader
    SkySHProjection_CS     = 36,  // Sky → L2 SH projection (9 float3 diffuse IBL)
    SpecularPrefilter_CS   = 37,  // GGX split-sum specular cubemap pre-filter (one mip per dispatch)
    SkyAtmosphere_CS       = 38,  // Hillaire atmospheric cubemap generator (samples SkyView LUT)
    AtmosphereTransmittance_CS = 39,  // Hillaire Transmittance LUT (256×64, once)
    AtmosphereMultiScatter_CS  = 40,  // Hillaire MultiScatter LUT (32×32, once)
    AtmosphereSkyView_CS       = 41,  // Hillaire SkyView LUT (192×108, per-frame)
    AerialPerspective_CS       = 42,  // Hillaire Aerial Perspective 3D LUT (32×32×32, per-frame)
    FroxelDensity_CS           = 43,  // Volumetric fog: per-froxel medium density
    FroxelLightInject_CS       = 44,  // Volumetric fog: per-froxel light injection
    FroxelScatter_CS           = 45,  // Volumetric fog: front-to-back integration
    VolumetricApply_VS         = 46,  // Volumetric fog: composite fullscreen tri (VS)
    VolumetricApply_PS         = 47,  // Volumetric fog: composite (PS)
    FroxelTemporal_CS          = 48,  // Volumetric fog: temporal reprojection ping-pong
    SceneVoxelize_CS           = 49,  // Scene occupancy voxelization for volumetric-light occlusion
    SceneVoxelClear_CS         = 50,  // Zeroes the occupancy grid at the start of each voxelize frame
    VolumetricRaymarch_CS      = 51,  // Per-pixel (half-res) raymarch for sun + shadow-casting spots
    VolumetricRaymarchTemporal_CS = 52,  // Temporal reprojection of the half-res raymarch output
    VolumetricRaymarchApply_PS = 53,  // Composite PS: half-res raymarch → HDR (additive)
    XeGTAODepthLinearize_CS    = 54,  // XeGTAO: hardware depth → linearized
    StarsBake_CS               = 55,  // One-time bake of the night-sky star cubemap
    ParticleEmit_CS            = 56,  // Particle system: emit new particles into pool
    ParticleUpdate_CS          = 57,  // Particle system: advance all alive particles
    Particle_VS                = 58,  // Particle system: billboard VS
    Particle_PS                = 59,  // Particle system: radial-falloff sprite PS
    TrailUpdate_CS             = 60,  // Trail system: append + age segments
    Trail_VS                   = 61,  // Trail system: ribbon extrusion VS
    Trail_PS                   = 62,  // Trail system: flat color + age alpha
    XeGTAOTemporal_CS          = 63,  // XeGTAO: AO-only temporal accumulation pass
    CAS_CS                     = 64,  // AMD FidelityFX Contrast Adaptive Sharpening
    ProbeCapture_PS            = 65,  // Reflection-probe baker forward PS (sun + SH ambient → 1 RTV)
    ProbeCaptureSky_PS         = 66,  // Reflection-probe baker sky PS (sample one cubemap, no sun disk)
    ClusterCullProbes_CS       = 67,  // Cluster culling for reflection probes (mirrors ClusterCull layout)
    HiZReduce_CS               = 68,  // Hi-Z mip N = reduce(mip N-1) — UAV→UAV, no per-subresource transitions
    SSRTrace_CS                = 69,  // Screen-space reflection trace (linear march, mirror; e1 baseline)
    SSRComposite_CS            = 70,  // SSR composite — blend hit-UV reflection into HDR (e3)
    SSRDepthHierarchyMip0_CS   = 71,  // SSR depth hierarchy mip 0 (depth SRV → R16G16 max/min)
    SSRDepthHierarchyReduce_CS = 72,  // SSR depth hierarchy mip N = reduce(mip N-1) — UAV→UAV
    SSRResolve_CS              = 73,  // SSR spatial reuse + variance + reprojection-depth
    SceneColorPyramidMip0_CS   = 74,  // HDR pre-filtered pyramid mip 0 (snapshot → UAV copy)
    SceneColorPyramidReduce_CS = 75,  // HDR pre-filtered pyramid reduce (Karis firefly-weighted)
    SSRTemporal_CS             = 76,  // SSR temporal reprojection + YCoCg variance clip
    SSRUpsample_CS             = 77,  // SSR variance-driven bilateral blur (final stage)
    DecalClusterCull_CS        = 78,  // Clustered decals: assign decal OBBs to clusters
    DecalApply_CS              = 79,  // Clustered decals: per-pixel blend into GBuffer via UAV
    GlassShatterInit_CS        = 80,  // Glass shatter: per-shard physics initial state
    GlassShatterSimulate_CS    = 81,  // Glass shatter: per-frame physics integration
    GlassShatterComposite_CS   = 82,  // Glass shatter: per-pixel polygon test + frozen-source sample
    TracerEmit_CS              = 83,  // Tracer system: copy CPU spawn queue into GPU pool
    TracerUpdate_CS            = 84,  // Tracer system: age all alive tracers
    Tracer_VS                  = 85,  // Tracer system: cylindrical-billboard quad VS
    Tracer_PS                  = 86,  // Tracer system: scrolling-noise + soft-fade PS
    BeamTubeGen_CS             = 87,  // Heavy-beam: Parallel-transport tube vertex generation
    Terrain_MS                 = 88,  // Mesh-shader terrain: per-tile vertex + index emission
    Terrain_PS                 = 89,  // Mesh-shader terrain: GBuffer output (slope-based debug colour, Phase 1)
    Terrain_Shadow_MS          = 90,  // Mesh-shader terrain: depth-only for CSM cascades
    Terrain_AS                 = 91,  // Amplification shader: per-sub-tile frustum cull (colour pass)
    Terrain_Shadow_AS          = 92,  // Amplification shader: dispatch fan-out only (shadow pass — no cull yet)
    UI_VS                      = 93,  // UI: orthographic quad VS, ByteAddressBuffer vertex fetch
    UI_PS                      = 94,  // UI: textured + tint PS
    WorldUI_VS                 = 95,  // World-space UI billboard VS (3D quads, viewProj)
    WorldUI_PS                 = 96,  // World-space UI textured + tint PS
    LensFlare_CS               = 97,  // Procedural directional-light lens flare (additive HDR)
    FXAA_CS                    = 98,  // NVIDIA FXAA 3.11 quality preset, single-pass HDR-aware
    OutlineObjectID_VS         = 99,  // Minimal PVF VS for outline ObjectID sub-pass (un-jittered VP)
    AfterimageCopy_CS          = 100, // Afterimage: copy skinned pos/nrm slice into snapshot pool
    CloudNoiseBake_CS          = 101, // Volumetric clouds: bake 128^3 Worley/Perlin noise once
    CloudRaymarch_CS           = 102, // Volumetric clouds: quarter-res raymarch through cloud slab
    CloudComposite_VS          = 103, // Volumetric clouds: fullscreen-triangle VS
    CloudComposite_PS          = 104, // Volumetric clouds: bilinear upsample + alpha-over composite
    VideoComposite_VS          = 105, // Video playback: fullscreen-triangle VS
    VideoComposite_PS          = 106, // Video playback: NV12 Y/UV plane → linear RGB composite
    VideoQuad_VS               = 107, // Video playback: world-space quad VS (procedural 6-vert)
    VideoQuad_PS               = 108, // Video playback: NV12 sample for world-space video quad
    Count,
};

class ShaderLibrary
{
public:
    // shaderDir: directory containing .hlsl source and .ishdr cache files.
    //            Must end with '/' or '\\'.  e.g. "shaders/"
    void Init(IGraphicsDevice& gfx, const char* shaderDir);
    void Shutdown(IGraphicsDevice& gfx);

    // Register a shader slot before calling GetShader or PreloadShader.
    // hlslFile: filename inside shaderDir (e.g. "GBuffer.vs.hlsl")
    // entry:    HLSL entry point name (default "main")
    void Register(ShaderID id, RHI::ShaderStage stage,
                  const char* hlslFile, const char* entry = "main");

    // Register a runtime-discovered shader (e.g. a user-authored custom PS
    // in a material). Returns a dynamic ID >= ShaderID::Count that
    // GetDynamicShader / GetDynamicReflection accept. Re-registering the
    // same (hlslFile, stage, entry) triple returns the existing ID — safe
    // to call once per frame from material setup code.
    //
    // hlslFile is relative to shaderDir (same convention as Register), or
    // an absolute path if it starts with a drive letter. Returns the
    // sentinel kInvalidDynShaderID on failure.
    static constexpr uint32_t kInvalidDynShaderID = ~0u;
    uint32_t RegisterDynamic(const char* hlslFile, RHI::ShaderStage stage,
                             const char* entry = "main");

    // Returns the RHI::Shader handle for (id, permutation).
    // Compiles + caches on first call; O(1) thereafter.
    // Returns nullptr on failure.
    const RHI::Shader* GetShader(ShaderID id, PermutationKey perm = {});

    // Dynamic-shader overload. dynId must come from RegisterDynamic.
    const RHI::Shader* GetDynamicShader(uint32_t dynId, PermutationKey perm = {});

    // Reflection info for (id, permutation), populated alongside GetShader.
    // Returns nullptr if the shader has never been requested, if the
    // underlying compile/load failed, or if reflection rejected the container.
    // Pointer is stable for the lifetime of the ShaderLibrary.
    const ShaderReflect::Reflection* GetReflection(ShaderID id, PermutationKey perm = {});
    const ShaderReflect::Reflection* GetDynamicReflection(uint32_t dynId, PermutationKey perm = {});

    // Explicitly warm up a specific (id, perm) pair at load time.
    bool PreloadShader(ShaderID id, PermutationKey perm = {});

    // Drop every cached compiled shader + reflection + compile-failure
    // record. The static and dynamic registry tables (id ↔ source path /
    // entry / stage) are PRESERVED, so the next GetShader / GetReflection
    // call will recompile from disk via DXC. Hot-reload calls this when an
    // .hlsl/.hlsli file changes — that's also when negative-cached
    // failures should get a chance to retry.
    //
    // GPU-side RHI::Shader handles that callers already cached are NOT
    // invalidated by this call — they still point at their old bytecode in
    // the GPU device's shader pool. Callers that hold those handles must be
    // told to re-fetch (typically by also clearing PSOCache and re-running
    // the pass's Init).
    void ClearCaches();

    // Last DXC error string for (id, perm). Returns the diagnostic text DXC
    // emitted on the most recent failed compile attempt, or empty string if
    // the shader compiled clean / hasn't been requested yet. The value is
    // copied (not a reference) so the caller can use it without holding the
    // library's mutex.
    //
    // Used by the ShaderLab inspector to render a red "Compile Error" panel
    // when the user assigns a custom shader that fails to build.
    std::string GetCompileError       (ShaderID id,    PermutationKey perm = {});
    std::string GetDynamicCompileError(uint32_t dynId, PermutationKey perm = {});

private:
    struct ShaderEntry
    {
        std::string      hlslFile;
        std::string      entry;
        RHI::ShaderStage stage = RHI::ShaderStage::Count;
        bool             registered = false;
    };

    struct CacheKey
    {
        uint32_t shaderID;
        uint32_t permBits;
        bool operator==(const CacheKey& o) const noexcept
        { return shaderID == o.shaderID && permBits == o.permBits; }
    };
    struct CacheKeyHash
    {
        size_t operator()(const CacheKey& k) const noexcept
        {
            // FNV-1a 64-bit combine
            uint64_t h = 14695981039346656037ull;
            h ^= static_cast<uint64_t>(k.shaderID);  h *= 1099511628211ull;
            h ^= static_cast<uint64_t>(k.permBits);  h *= 1099511628211ull;
            return static_cast<size_t>(h);
        }
    };

    std::string      m_shaderDir;
    // Directory where compiled .ishdr blobs land. Lives INSIDE shaders/ so
    // the whole shader ecosystem stays in one folder, but segregated from
    // the .hlsl source files. CachePath strips the shaderDir prefix from
    // the source's parent to avoid "shaders/shader_cache_dxil/shaders/..." — so
    // "shaders/GBuffer.ps.hlsl" caches to
    // "shaders/shader_cache_dxil/GBuffer.ps_main_P00000001.ishdr".
    //
    // Bumped from "shader_cache/" to "shader_cache_dxil/" when we migrated
    // the compiler from D3DCompile (DXBC, SM 5.1) to DXC (DXIL, SM 6.6) — the
    // bytecode formats are not interchangeable and we don't want a half-cold
    // checkout to silently load stale DXBC blobs into a SM 6.6 PSO.
    std::string      m_cacheDir = "shaders/shader_cache_dxil/";
    IGraphicsDevice* m_gfx = nullptr;

    ShaderEntry m_registry[static_cast<uint32_t>(ShaderID::Count)];
    // Dynamic entries live past the fixed-enum range. IDs are
    // (ShaderID::Count + dynIndex) so the cache map (keyed by uint32_t)
    // needs no structural change.
    std::vector<ShaderEntry> m_dynamicRegistry;
    // "path|entry|stage" → dynamic ID. Enforces dedup so repeated
    // RegisterDynamic calls for the same shader are cheap.
    std::unordered_map<std::string, uint32_t> m_dynamicPathToId;

    std::unordered_map<CacheKey, RHI::Shader, CacheKeyHash> m_shaderCache;
    // Parallel map keyed the same way. Separate from m_shaderCache so the
    // hot per-frame GetShader path stays tight; reflection is only consulted
    // by editor UI / material binding setup.
    std::unordered_map<CacheKey, ShaderReflect::Reflection, CacheKeyHash> m_reflectionCache;
    // Negative cache: (id, perm) → DXC error message for the last failed
    // compile attempt. GetShaderInternal short-circuits when a key is
    // present so a broken shader doesn't re-trigger DXC every frame and
    // spam the log. ClearCaches drops this map alongside the others, so
    // hot-reload (which fires when the source file is edited) gives the
    // shader another chance.
    std::unordered_map<CacheKey, std::string, CacheKeyHash> m_failedShaders;
    std::mutex m_mutex;

    // Shared GetShader/GetReflection implementation — accepts raw uint32_t so
    // static ShaderID and dynamic IDs flow through the same code path.
    const RHI::Shader* GetShaderInternal(uint32_t id, PermutationKey perm);
    const ShaderReflect::Reflection* GetReflectionInternal(uint32_t id, PermutationKey perm);

    // Returns the registry entry for a shader id (static or dynamic); nullptr
    // if the id is out of range or not registered.
    const ShaderEntry* GetRegistryEntry(uint32_t id) const;

    // Internal: compile (or load cached) bytecode and upload to GPU.
    // outBytecode receives the raw DXIL container so callers (e.g. GetShader)
    // can feed it to the shader reflection layer without re-reading the cache
    // blob. Returns true on success; outShader and outBytecode are set.
    bool LoadOrCompile(uint32_t id, PermutationKey perm,
                       RHI::Shader& outShader,
                       std::vector<uint8_t>& outBytecode);

    // Derive the .ishdr cache file path for a given source file + permutation.
    std::string CachePath(const std::string& hlslFile, PermutationKey perm, const std::string& entryPoint) const;

    // Read the .ishdr blob from disk. Returns empty vector if file not found.
    static std::vector<uint8_t> ReadFile(const std::string& path);

    // Write bytes to disk (creates file; overwrites if exists).
    static bool WriteFile(const std::string& path, const std::vector<uint8_t>& data);

    // Build D3D_SHADER_MACRO list from PermutationKey.
    // Returns a null-terminated array (last element is {nullptr,nullptr}).
    static std::vector<std::pair<std::string,std::string>> MakeDefines(PermutationKey perm);
};
