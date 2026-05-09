#include "PostProcess/PostProcessStack.h"
#include "PostProcess/IVolumeSource.h"

#include <algorithm>
#include <cassert>

namespace PostProcess
{

void Stack::RegisterEffect(std::unique_ptr<IEffect> effect)
{
    assert(effect && "RegisterEffect: null effect");
    const size_t slot = static_cast<size_t>(effect->GetStage());
    assert(slot < static_cast<size_t>(Stage::Count));
    m_effects[slot] = std::move(effect);
}

void Stack::AddVolumeSource(IVolumeSource* src)
{
    if (!src) return;
    // Guard against double-register — a no-op rather than an error so
    // re-Init paths are safe.
    if (std::find(m_volumeSources.begin(), m_volumeSources.end(), src)
        != m_volumeSources.end()) return;
    m_volumeSources.push_back(src);
}

void Stack::RemoveVolumeSource(IVolumeSource* src)
{
    m_volumeSources.erase(
        std::remove(m_volumeSources.begin(), m_volumeSources.end(), src),
        m_volumeSources.end());
}

void Stack::Execute(Context& ctx)
{
    // Gather snapshots from every override producer into one list, then sort
    // globally by priority so the blender walks a single monotonically
    // layered sequence.
    std::vector<Snapshot> snapshots = m_scriptedOverrides.Tick(ctx.deltaTime);

    for (IVolumeSource* src : m_volumeSources)
    {
        if (src) src->Gather(ctx, snapshots);
    }

    if (snapshots.empty())
    {
        // Fast path — no overrides: blended == base.
        m_blendedParams.GetCAS()          = m_baseParams.GetCAS();
        m_blendedParams.GetAutoExposure() = m_baseParams.GetAutoExposure();
        m_blendedParams.GetBloom()        = m_baseParams.GetBloom();
        m_blendedParams.GetTonemapping()  = m_baseParams.GetTonemapping();
    }
    else
    {
        std::stable_sort(snapshots.begin(), snapshots.end(),
            [](const Snapshot& a, const Snapshot& b)
            { return a.priority < b.priority; });
        m_blender.Blend(m_baseParams, snapshots, m_blendedParams);
    }

    ctx.params = &m_blendedParams;

    for (auto& effect : m_effects)
    {
        if (!effect) continue;
        if (!effect->IsEnabled(ctx)) continue;
        effect->Execute(ctx);
    }
}

} // namespace PostProcess
