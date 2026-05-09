#include "PostProcess/VolumeSystem.h"

using namespace DirectX;

namespace PostProcess
{

// ---------------------------------------------------------------------------
// Slot-list registration
// ---------------------------------------------------------------------------
VolumeHandle VolumeSystem::Register(const Volume& v)
{
    if (!m_freeList.empty())
    {
        const uint32_t slot = m_freeList.back();
        m_freeList.pop_back();
        m_slots[slot].occupied = true;
        m_slots[slot].volume   = v;
        return slot;
    }

    const uint32_t slot = static_cast<uint32_t>(m_slots.size());
    m_slots.push_back({ /*occupied=*/true, v });
    return slot;
}

void VolumeSystem::Unregister(VolumeHandle h)
{
    if (h >= m_slots.size() || !m_slots[h].occupied) return;
    m_slots[h].occupied = false;
    m_slots[h].volume   = Volume{};  // drop any override payload
    m_freeList.push_back(h);
}

Volume* VolumeSystem::Get(VolumeHandle h)
{
    if (h >= m_slots.size() || !m_slots[h].occupied) return nullptr;
    return &m_slots[h].volume;
}

const Volume* VolumeSystem::Get(VolumeHandle h) const
{
    if (h >= m_slots.size() || !m_slots[h].occupied) return nullptr;
    return &m_slots[h].volume;
}

// ---------------------------------------------------------------------------
// Gather — IVolumeSource implementation
// ---------------------------------------------------------------------------
void VolumeSystem::Gather(const Context& ctx, std::vector<Snapshot>& out)
{
    out.reserve(out.size() + m_slots.size());

    for (const Slot& s : m_slots)
    {
        if (!s.occupied)                   continue;
        const Volume& v = s.volume;
        if (!v.enabled)                    continue;
        if (!v.override.HasAnyOverride())  continue;  // nothing to contribute

        const float sd = ComputeSignedDistance(v, ctx.cameraPos);
        const float w  = ComputeWeightFromDistance(sd, v.blendDistance);
        if (w <= 0.0f) continue;

        out.push_back(Snapshot{ w, v.priority, &v.override });
    }
    // Sorting is done globally by Stack after merging all sources.
}

} // namespace PostProcess
