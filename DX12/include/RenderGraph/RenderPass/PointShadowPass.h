#pragma once

// PointShadowPass — omnidirectional (cubemap) shadow maps for POINT lights
// that opt-in via LightData::castsShadow == true. The mirror of SpotShadowPass,
// but each caster owns a full cube (6 faces) instead of a single slice.
//
// Storage: one D32_FLOAT TextureCubeArray. Cube `c` occupies array slices
// [c*6 .. c*6+5] in the canonical D3D face order (+X,-X,+Y,-Y,+Z,-Z). A depth
// TextureCubeArray<float> SRV is produced by the RHI automatically because the
// texture carries ResourceMiscFlag::TEXTURECUBE and array_size == 6*N
// (see GraphicsDX12::CreateTexture). Per-face DSVs are the same per-slice DSVs
// the RHI auto-creates for any array depth texture, so SetDepthStencilSlice /
// ClearDepthStencilSlice address face `f` of cube `c` as slice `c*6 + f`.
//
// Each frame:
//   1. Renderer collects point lights with castsShadow, assigns them a cube
//      index, and builds six per-face view-projection matrices (90° fov,
//      reversed-Z perspective, near = radius / far = kNearPlane — matching the
//      SpotShadowPass convention so the same Shadow.vs/ps + comparison sampler
//      work unchanged).
//   2. This pass clears every face, then renders all opaque DrawPackets
//      depth-only once per face (6 * activeCount scene passes).
//   3. Lighting.ps samples the cube by the world-space direction
//      (worldPos - lightPos) and compares against a reversed-Z reference depth
//      reconstructed analytically from the fragment's distance to the light
//      (SamplePointShadow). Lights whose shadowSliceIdx == 0xFFFFFFFF bypass
//      the sampling entirely (no shadow — original behaviour).
//
// Reuses Shadow_VS / Shadow_PS (same depth-only pair CSM + SpotShadow use).
//
// Root-sig layout (runtime, identical to SpotShadowPass):
//   b1 space0 — ShadowPerViewCB (per-face shadowViewProj)
//   t0 space0 — InstanceBuffer
//   t1 space0 — MeshDescriptors
//   t0 space1 — bindless g_Buffers[]

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include "Graphics/IndirectDrawCommand.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class PointShadowPass : public RG::RenderPass
{
public:
    static constexpr uint32_t kMaxCasters   = 4;       // cubes (each = 6 faces)
    static constexpr uint32_t kFacesPerCube = 6;
    static constexpr uint32_t kShadowMapSize = 1024;   // per face
    static constexpr uint32_t kInvalidSliceIdx = 0xFFFFFFFFu;
    // Reversed-Z near plane shared with Lighting.ps SamplePointShadow. The
    // far plane is the light's radius (per-caster). Keep both sides in sync.
    static constexpr float    kNearPlane = 0.05f;

    PointShadowPass();
    ~PointShadowPass();

    const char* GetName() const override { return "PointShadowPass"; }
    void Setup (RG::RenderGraphBuilder&) override {}
    void Init  (IGraphicsDevice& gfx)    override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Renderer calls these each frame BEFORE graph Execute.
    // activeCount = number of point lights that opted-in this frame (<= kMaxCasters).
    void SetActiveCasterCount(uint32_t n) { m_activeCount = (n > kMaxCasters) ? kMaxCasters : n; }
    void SetFaceMatrix(uint32_t cube, uint32_t face, const DirectX::XMFLOAT4X4& viewProj)
    {
        if (cube < kMaxCasters && face < kFacesPerCube)
            m_pendingVP[cube * kFacesPerCube + face] = viewProj;
    }

    // Consumed by Lighting.ps as a TextureCubeArray<float>.
    uint64_t                  GetCubeAtlasSrvHandle() const;
    static constexpr uint32_t GetAtlasSize()  { return kShadowMapSize; }
    static constexpr uint32_t GetMaxCasters() { return kMaxCasters; }

    const RHI::Texture& GetAtlasTexture() const { return m_atlas; }

private:
    PSODesc BuildPSODesc(PermutationKey perm) const;
    void    UploadFaceCBs();

    ShaderLibrary m_shaderLib;
    PSOCache      m_psoCache;

    RHI::Texture m_atlas;  // Texture2DArray<D32_FLOAT> w/ TEXTURECUBE → TextureCubeArray SRV

    // One 256-byte CBV slot per face (kMaxCasters * 6 faces). Triple-buffered.
    static constexpr uint32_t kFaceCount     = kMaxCasters * kFacesPerCube;
    static constexpr uint32_t kFaceCBStride  = 256; // D3D12 CBV alignment
    struct FaceCBPool
    {
        uint8_t slots[kFaceCount * kFaceCBStride];
    };
    FrameCB<FaceCBPool> m_faceCBs;

    DirectX::XMFLOAT4X4 m_pendingVP[kFaceCount]{};
    uint32_t            m_activeCount = 0;

    // Indirect draw arg buffer for batching opaque draws (same layout as
    // SpotShadowPass / ShadowPass). Built once per frame, reused across all
    // faces. Triple-buffered manual ring (UPLOAD heap). 3 == FrameCount.
    static constexpr uint32_t kMaxIndirectCommands = 32768;
    static constexpr uint32_t kFrameCount          = 3;
    RHI::GPUBuffer m_indirectArgBuffer[kFrameCount];
    void*          m_indirectArgMapped[kFrameCount] = {};

    IGraphicsDevice* m_gfxPtr = nullptr;

    // Linear-wrap sampler for alpha-test shadow PS (matches GBuffer.ps's g_LinearWrap).
    int  m_alphaSamplerIdx = -1;

    // Transition from DEPTHSTENCIL on the first frame, DEPTH_READ_SRV after.
    bool m_firstExecution = true;
};
