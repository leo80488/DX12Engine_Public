#pragma once

// BillboardFXPass — renders BillboardFXComponent entities as animated,
// camera-facing sprite-sheet quads into the HDR scene colour (pre-tonemap),
// with read-only depth test so scene geometry occludes them and Bloom picks up
// emissive / additive glow.
//
// Self-contained, like WorldUIBillboardPass: the pass CPU-expands a camera-
// facing quad per effect and bakes the current flipbook frame's atlas sub-rect
// straight into a triple-buffered UPLOAD vertex ring.  HDR + depth binding
// mirror TransparentPass (SetRenderTargetToHdrWithDepth, reversed-Z depth test,
// depth-write OFF).
//
// Lifetime split:
//   BuildFrame(world, dt, ...)  — called by Renderer each frame BEFORE graph
//                                 execute (needs World + dt to advance the
//                                 flipbook and resolve textures).
//   Execute(cl)                 — graph node: binds the ring and draws.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Resource/TextureSystem.h"   // Resource::TextureHandle / kInvalidTextureHandle
#include "ECS/BillboardFXComponent.h"

#include <DirectXMath.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>

class World;
namespace Resource { class ResourceManager; }

class BillboardFXPass : public RG::RenderPass
{
public:
    explicit BillboardFXPass(RG::RGTextureHandle depth);

    const char* GetName() const override { return "BillboardFXPass"; }
    void Setup  (RG::RenderGraphBuilder& b)       override;
    void Init   (IGraphicsDevice& gfx)            override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Texture-path → bindless-index resolution (optional; null = untextured).
    void SetResourceSystems(Resource::TextureSystem* texSys,
                            Resource::ResourceManager* resMgr)
    { m_texSys = texSys; m_resMgr = resMgr; }

    // CPU prep: advance flipbooks, resolve textures, expand camera-facing quads
    // into this frame's vertex ring + record draw ranges.  Called by Renderer
    // before graph execute (has World + dt + camera).
    // viewProj is NOT passed — the VS reads it from the engine's shared
    // "PerView" CB (b1), same as GBuffer.vs. We only need the view matrix
    // (camera right/up axes) + camera position (back-to-front sort).
    void BuildFrame(World& world, float dt,
                    const DirectX::XMFLOAT4X4& viewMatrix,
                    const DirectX::XMFLOAT3&   cameraPos);

    // Texture-cache teardown (mirrors ParticleSystem). Releases the cached
    // texture handle(s) so a recycled Entity ID never inherits a stale bindless
    // slot and texture refcounts stay balanced. Wired from Renderer:
    // OnEntityDestroyed via the destroy-listener fan-out, OnWorldClear on reload
    // (World::Clear does NOT fire per-entity destroy listeners). TextureSystem::
    // Release defers the GPU destruction, so calling it mid-frame is safe.
    void OnEntityDestroyed(uint32_t entity);
    void OnWorldClear();

    bool enabled = true;

    // 40-byte vertex × 6 per quad; 24576 verts = 4096 quads / frame.
    static constexpr uint32_t kMaxVertices = 24576;

private:
    // 40-byte vertex: pos(12) + uv(8) + HDR colour float4(16) + texIdx(4).
    struct Vertex
    {
        float    pos[3];
        float    uv[2];
        float    color[4];
        uint32_t texIdx;
    };
    static_assert(sizeof(Vertex) == 40, "BillboardFX Vertex must be 40 bytes");

    static constexpr uint32_t kInvalidTexIdx = 0xFFFFFFFFu;

    // One DrawInstanced range = a run of consecutive sorted quads that share a
    // (blend, depthTest) PSO.  Painter order is preserved across the run.
    struct DrawRange
    {
        uint32_t         startVertex;
        uint32_t         count;
        BillboardFXBlend blend;
        bool             depthTest;
    };

    PSODesc BuildPSODesc(BillboardFXBlend blend, bool depthTest) const;

    static void EmitQuad(std::vector<Vertex>& out,
                         const DirectX::XMFLOAT3& center,
                         const DirectX::XMFLOAT3& right,
                         const DirectX::XMFLOAT3& up,
                         float halfW, float halfH,
                         float u0, float v0, float u1, float v1,
                         const float color[4], uint32_t texIdx);

    // Resolve the component's texturePath → bindless index (mirrors
    // ParticleSystem): cache per entity, Release old on path change, promote to
    // bindless idx once the async load is ready.
    void ResolveTexture(uint32_t entity, BillboardFXComponent& b);

    IGraphicsDevice*    m_gfx = nullptr;
    RG::RGTextureHandle m_depth;
    ShaderLibrary       m_shaderLib;
    PSOCache            m_psoCache;

    static constexpr uint32_t kFrameCount = 3;     // == GraphicsDX12::FrameCount
    RHI::GPUBuffer m_vertexBuffer[kFrameCount];
    void*          m_vbMapped[kFrameCount] = {};
    int            m_samplerIdx = -1;

    std::vector<DrawRange> m_draws;

    // Texture resolution dependencies (optional — null = no texture support).
    Resource::TextureSystem*   m_texSys = nullptr;
    Resource::ResourceManager* m_resMgr = nullptr;
    struct TexCache { std::string path; Resource::TextureHandle handle = Resource::kInvalidTextureHandle; };
    std::unordered_map<uint32_t, TexCache> m_texCache;
};
