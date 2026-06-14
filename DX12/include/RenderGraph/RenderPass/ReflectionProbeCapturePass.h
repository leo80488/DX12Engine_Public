#pragma once

// ReflectionProbeCapturePass — turns the Renderer's draw packets into a baked
// reflection cubemap for one probe at a time. Pipeline per probe:
//   (1) Render scene 6 times into a shared 128×128×6 temp TextureCube using
//       a simplified forward shader (sky + opaque, sun lambert + SH ambient).
//   (2) Dispatch GGX prefilter compute 7 mips × 6 faces = 42 dispatches,
//       reading the temp cube and writing into one cube of the renderer's
//       reflection-probe TextureCubeArray (slice [cubeIdx*6 .. cubeIdx*6+5]).
//
// The pass is NOT registered with the RenderGraph — Renderer drives it
// explicitly so it can amortize the cost across frames (one probe per frame
// by default). Bake target slice + scene state are passed via BakeContext.

#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/RenderTypes.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class ReflectionProbeCapturePass
{
public:
    static constexpr uint32_t kFaceSize = 128;
    static constexpr uint32_t kFaceMips = 7;  // log2(128) + 1

    struct BakeContext
    {
        RHI::Texture*           probeArray         = nullptr;  // destination cubemap-array
        DirectX::XMFLOAT3       probePos           { 0, 0, 0 }; // capture origin
        DirectX::XMFLOAT3       sunDir             { 0, -1, 0 };
        DirectX::XMFLOAT3       sunColor           { 1, 1, 1 };
        uint64_t                skySHSrv           = 0;        // t19 space0
        uint64_t                materialBufSrv     = 0;        // t2 space0
        uint64_t                bindlessTexTable   = 0;        // t0 space2
        uint64_t                bindlessBufTable   = 0;        // t0 space1 (PVF g_Buffers[])
        const RHI::GPUBuffer*   instanceBuffer     = nullptr;  // root SRV slot 8 (t0 space0)
        const RHI::GPUBuffer*   meshDescBuffer     = nullptr;  // root SRV slot 9 (t1 space0)
        DrawList                opaqueDraws;                    // passed camera frustum
        // Shadow-only draws — entities that FAILED the camera frustum test but
        // passed a CSM cascade (built by BuildScene_CullAndCollect). These are
        // drawn too so probes capture geometry behind the main camera (e.g. the
        // ceiling from inside a room looking forward). Iterated with the same
        // capture PSO as opaqueDraws.
        DrawList                shadowDraws;
        // Transparent draws — alpha-blend materials. BuildScene_CullAndCollect
        // now rescues these from the camera-cull drop so probes can see them.
        // Drawn with the SAME opaque capture PSO (no alpha blend) — probes
        // capture transparent surfaces as if they were solid, which is what
        // AAA engines typically do for baked / low-res reflection probes.
        DrawList                transparentDraws;
        uint64_t                defaultWhiteSrv    = 0;        // fallback for t3 (BaseColor)
        uint64_t                defaultFlatNormSrv = 0;        // fallback for t5 (NormalMap)
        // Skybox draw — fills pixels the opaque pass left untouched. Borrows
        // SkyboxPass's 36-vert cube VB (ByteAddressBuffer at t2 space0 in the
        // sky shader) and SkyIBLPass's currently active sky cubemap SRV.
        const RHI::GPUBuffer*   skyCubeVB          = nullptr;
        uint64_t                skyCubemapSrv      = 0;        // t6 space0

        // ---- Direct-sun shadowing for the bake (optional) -----------------
        // When shadowArraySrv != 0 the capture PS shadows the direct sun using
        // the main camera's CSM, so roofed interiors don't bake full sunlight.
        // Cascades are camera-relative, so a probe far outside the camera's
        // cascade range simply bakes unshadowed (graceful fallback). All values
        // are frame-global (identical for every probe baked this frame).
        uint64_t                shadowArraySrv     = 0;        // CSM Texture2DArray @ t9 space0 (root slot 22)
        DirectX::XMFLOAT4X4     cascadeVP[4]       {};         // transposed (matches LightCB.shadowMatrix)
        DirectX::XMFLOAT4       cascadeSplits      { 0,0,0,0 };
        DirectX::XMFLOAT3       camPos             { 0,0,0 };   // main camera (cascade selection)
        DirectX::XMFLOAT3       camFwd             { 0,0,1 };
        float                   shadowStrength     = 0.0f;     // 0 = no CSM this frame → bake unshadowed

        // ---- Indirect/ambient bake controls -------------------------------
        float                   iblStrength        = 1.0f;     // live sky-IBL master at bake time
        float                   ambientScale       = 1.0f;     // IndirectLightingSettings.reflectionProbeBakeAmbient
    };

    bool Init(IGraphicsDevice& gfx);

    // Bake one probe — records draws + dispatches onto cl. Caller is responsible
    // for transitioning the destination cubemap-array to UNORDERED_ACCESS before
    // calling and back to SHADER_RESOURCE after.
    void BakeProbe(RHI::CommandList cl, uint32_t cubeIdx, const BakeContext& ctx);

private:
    IGraphicsDevice* m_gfx = nullptr;

    ShaderLibrary       m_shaderLib;
    RHI::PipelineState  m_capturePSO;     // forward graphics PSO (1 RTV)
    RHI::PipelineState  m_skyPSO;         // skybox draw — fills sky pixels after opaque
    RHI::PipelineState  m_prefilterPSO;   // GGX prefilter compute (reused shape from SkyIBLPass)

    // 128×128 D32 depth for the per-face forward draw. Cleared per face.
    RHI::Texture       m_depthBuffer;
    RHI::ResourceState m_depthState = RHI::ResourceState::DEPTHSTENCIL;

    // 128×128×6 R16G16B16A16_FLOAT TextureCube — capture mip 0 destination,
    // prefilter source. Stays alive for the renderer's lifetime so the
    // descriptor allocations behind per-face RTVs can be reused.
    // CreateTexture forces RT-bound resources to start in RENDER_TARGET state
    // (see GraphicsDX12.cpp:2178), so the CPU tracker reflects that.
    RHI::Texture       m_tempCube;
    RHI::ResourceState m_tempCubeState = RHI::ResourceState::RENDERTARGET;

    // CB ring — capture path uploads 6 entries per probe (1 per face).
    // 256-byte aligned slots so root CBV addresses can offset into the buffer.
    RHI::GPUBuffer m_captureCB;
    void*          m_captureCBMapped = nullptr;

    // Prefilter CB ring — 42 entries per probe (7 mips × 6 faces).
    RHI::GPUBuffer m_prefilterCB;
    void*          m_prefilterCBMapped = nullptr;

    // Per-probe shadow/ambient CB (b2). Frame-global data (CSM cascades, camera,
    // iblStrength, ambientScale) — uploaded once per BakeProbe, bound on each
    // face. Single buffer is safe because the data is identical for every probe
    // baked in a frame.
    RHI::GPUBuffer m_shadowCB;
    void*          m_shadowCBMapped = nullptr;

    int m_linearSamplerIdx = -1;
    int m_shadowSamplerIdx = -1;  // comparison sampler at s2 (reversed-Z CSM)

    void DrawSceneFace(RHI::CommandList cl, uint32_t face, const BakeContext& ctx);
};
