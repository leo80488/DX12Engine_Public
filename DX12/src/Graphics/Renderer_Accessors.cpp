#include "Graphics/Renderer.h"

// engine graphics / backend
#include "Graphics/SSR/SSRSubsystem.h"

// render passes
#include "RenderGraph/RenderPass/SSRPass.h"
#include "RenderGraph/RenderPass/SSRDepthHierarchyPass.h"
#include "RenderGraph/RenderPass/SceneColorPyramidPass.h"
#include "RenderGraph/RenderPass/ToneMapPass.h"
#include "RenderGraph/RenderPass/GlassShatterPass.h"

using namespace DirectX;

// CPU-side cbuffer mirrors live in Renderer.h (RendererDetail namespace) so the
// triple-buffered FrameCB<T> members can be instantiated in the class layout.
// Layouts there MUST stay in sync with the matching HLSL.
using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;
using TerrainParamsCB = RendererDetail::TerrainParamsCB;
static_assert(sizeof(TerrainParamsCB) == 176,
    "TerrainParamsCB layout drift — sync Terrain.{ms,as,ps,shadow.ms,shadow.as}.hlsl + Renderer.h");

// Renderer_Accessors.cpp — split out of Renderer.cpp (one TU per Renderer subsystem).
// All members belong to class Renderer (declared in Graphics/Renderer.h).
// The include block mirrors Renderer.cpp so every cluster keeps compiling;
// trim per-TU later if desired.
// Final-output / SSR / glass-shatter accessors + forwarders.

uint64_t Renderer::GetFinalOutputSrvHandle() const
{
    if (m_toneMapPass) return m_toneMapPass->GetFinalOutputSrvHandle();
    return 0;
}

void Renderer::TriggerGlassShatter(float impactU, float impactV)
{
    if (m_glassShatterPass) m_glassShatterPass->Trigger(impactU, impactV);
}

// ===========================================================================
// SSR accessors — forward through SSRSubsystem so SSRDebugWindow + Renderer
// callers continue to see the same per-pass pointers.
// ===========================================================================
uint64_t Renderer::GetSSRResultSrv() const
{
    return m_ssrSubsystem ? m_ssrSubsystem->GetTraceResultSrv() : 0;
}

SSRPass* Renderer::GetSSRPass()
{
    return m_ssrSubsystem ? m_ssrSubsystem->GetTrace() : nullptr;
}
SSRResolvePass* Renderer::GetSSRResolvePass()
{
    return m_ssrSubsystem ? m_ssrSubsystem->GetResolve() : nullptr;
}
SSRTemporalPass* Renderer::GetSSRTemporalPass()
{
    return m_ssrSubsystem ? m_ssrSubsystem->GetTemporal() : nullptr;
}
SSRUpsamplePass* Renderer::GetSSRUpsamplePass()
{
    return m_ssrSubsystem ? m_ssrSubsystem->GetUpsample() : nullptr;
}
SSRCompositePass* Renderer::GetSSRCompositePass()
{
    return m_ssrSubsystem ? m_ssrSubsystem->GetComposite() : nullptr;
}
SSRDepthHierarchyPass* Renderer::GetSSRDepthHierPass()
{
    return m_ssrSubsystem ? m_ssrSubsystem->GetDepthHier() : nullptr;
}
SceneColorPyramidPass* Renderer::GetSceneColorPyramidPass()
{
    return m_ssrSubsystem ? m_ssrSubsystem->GetSceneColorPyr() : nullptr;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------
