#pragma once

// DDGIProbeDebugPass — renders one icosahedron sphere per probe at the probe's
// world-space position. The sphere's surface normal is used to sample the
// volume's irradiance atlas via octahedral mapping, so the colour you see on
// each sphere is exactly what a surface oriented in that direction would
// receive from indirect bounce. A red wall in the scene therefore lights the
// "+X side" of every probe red, etc. — the classic DDGI debug viz.
//
// Standalone pass owned by Renderer (manual Execute, mirrors DebugWirePass /
// SkinningPass pattern). Renders into HDR with depth test enabled so probes
// occlude / are occluded by scene geometry naturally.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"

#include <wrl.h>
#include <d3d12.h>
#include <cstdint>

namespace DDGI { class DDGIVolumeManager; }
class World;

class DDGIProbeDebugPass
{
public:
    bool Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx);
    bool ReloadShaders(IGraphicsDevice& gfx);

    // Per-frame execute. Iterates active volumes; only draws probes for those
    // with `debugDraw == true` in their DDGIVolumeComponent.
    //
    // @p depthTex must be the GBuffer depth buffer (read+test, no write).
    // @p perViewCB carries viewProj for the VS.
    void Execute(IGraphicsDevice&             gfx,
                 RHI::CommandList             cmd,
                 const RHI::Texture*          depthTex,
                 const RHI::GPUBuffer&        perViewCB,
                 DDGI::DDGIVolumeManager&     mgr,
                 World&                       world);

    // User-facing toggle (mirrors DebugWirePass conventions). Sphere radius
    // shown for each probe (world units) — defaults small so a 16³ grid
    // doesn't hide the scene.
    bool   enabled       = true;
    float  sphereRadius  = 0.10f;
    // Diagnostic visualisation mode (matches `g_DebugMode` in DDGIProbeDebug.ps.hlsl):
    //   0 = SH irradiance (default — what the lighting pass actually samples)
    //   1 = grid-coord colour (verify probe placement)
    //   2 = SH L0 magnitude as heat (verify SH has data in it)
    //   3 = SH L1 directionality (verify SH evolves frame-to-frame —
    //       freeze test: if every probe stays the same colour after a sun
    //       rotation, relight isn't writing).
    uint32_t debugMode = 0;

private:
    // Dedicated DDGI-debug root signature — kept tiny so it doesn't piggyback
    // on the engine's default rasterisation rootsig:
    //   [0] ROOT_CBV          b0 space0  (PerViewCB — viewProj only)
    //   [1] ROOT_CBV          b1 space0  (DDGIVolumeGPU — origin/spacing/...)
    //   [2] ROOT_CONSTANTS    b2 space0  (1×float — sphereRadius)
    //   [3] DESC_TABLE 1 SRV  t0 space0  (irradiance atlas)
    //   static sampler s0 space0 (linear clamp)
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;

    // PSO is fixed (single triangle list, depth-test, no blend) so we can
    // hold one cached pointer rather than going through PSOCache.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

    ShaderLibrary m_shaderLib;

    // Sub-buffer for sphereRadius push — populated each frame via root constants.
    bool m_ready = false;
};
