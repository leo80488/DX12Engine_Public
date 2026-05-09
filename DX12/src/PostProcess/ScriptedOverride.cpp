#include "PostProcess/ScriptedOverride.h"

#include <algorithm>

namespace PostProcess
{

ScriptedOverrideHandle ScriptedOverrideSystem::Push(const ScriptedOverrideDesc& desc)
{
    if (!m_freeList.empty())
    {
        const uint32_t slot = m_freeList.back();
        m_freeList.pop_back();
        m_entries[slot].occupied    = true;
        m_entries[slot].desc        = desc;
        m_entries[slot].elapsedTime = 0.0f;
        return slot;
    }
    const uint32_t slot = static_cast<uint32_t>(m_entries.size());
    m_entries.push_back(Entry{ /*occupied=*/true, desc, 0.0f });
    return slot;
}

void ScriptedOverrideSystem::Pop(ScriptedOverrideHandle h)
{
    if (h >= m_entries.size() || !m_entries[h].occupied) return;
    m_entries[h].occupied = false;
    m_entries[h].desc     = ScriptedOverrideDesc{};  // drop override payload
    m_freeList.push_back(h);
}

// Piecewise-linear fade curve. Holds at 1 during `hold`, ramps 0→1 over
// `fadeIn` and 1→0 over `fadeOut`. Small epsilons guard divide-by-zero when
// a phase is set to 0 (instantaneous pulse).
float ScriptedOverrideSystem::EvaluateWeight(const ScriptedOverrideDesc& d, float elapsed)
{
    constexpr float kEps = 1.0e-4f;
    if (elapsed <= 0.0f) return d.fadeIn > kEps ? 0.0f : 1.0f;

    if (elapsed < d.fadeIn)
        return elapsed / std::max(d.fadeIn, kEps);
    float t = elapsed - d.fadeIn;

    if (t < d.hold)
        return 1.0f;
    t -= d.hold;

    if (t < d.fadeOut)
        return 1.0f - (t / std::max(d.fadeOut, kEps));

    return 0.0f;
}

std::vector<Snapshot> ScriptedOverrideSystem::Tick(float dt)
{
    std::vector<Snapshot> out;
    out.reserve(m_entries.size());

    for (uint32_t i = 0; i < m_entries.size(); ++i)
    {
        Entry& e = m_entries[i];
        if (!e.occupied) continue;

        e.elapsedTime += dt;
        const float total = TotalDuration(e.desc);

        if (e.elapsedTime >= total)
        {
            // Expired — retire the slot. Don't emit a snapshot: weight would
            // be 0 anyway, so blender would skip it.
            e.occupied = false;
            e.desc     = ScriptedOverrideDesc{};
            m_freeList.push_back(i);
            continue;
        }

        const float w = EvaluateWeight(e.desc, e.elapsedTime);
        if (w <= 0.0f || !e.desc.override.HasAnyOverride()) continue;

        out.push_back(Snapshot{
            /*weight=*/   w,
            /*priority=*/ e.desc.priority,
            /*override=*/ &e.desc.override,
        });
    }

    std::stable_sort(out.begin(), out.end(),
        [](const Snapshot& a, const Snapshot& b)
        { return a.priority < b.priority; });

    return out;
}

std::vector<ScriptedOverrideSystem::LiveEntry> ScriptedOverrideSystem::GetLiveEntries() const
{
    std::vector<LiveEntry> out;
    out.reserve(m_entries.size());
    for (uint32_t i = 0; i < m_entries.size(); ++i)
    {
        const Entry& e = m_entries[i];
        if (!e.occupied) continue;
        out.push_back(LiveEntry{
            /*handle=*/         i,
            /*elapsedSeconds=*/ e.elapsedTime,
            /*totalDuration=*/  TotalDuration(e.desc),
            /*priority=*/       e.desc.priority,
            /*label=*/          e.desc.label,
        });
    }
    return out;
}

} // namespace PostProcess
