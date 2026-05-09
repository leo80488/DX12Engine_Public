#pragma once

// PostProcess::VolumeSystem — slot-indexed registry of Volumes, one instance
// typically owned by Renderer. Implements IVolumeSource so PostProcess::Stack
// pulls snapshots through the same interface as ECS-driven sources.

#include "PostProcess/Volume.h"   // Volume, Snapshot
#include "PostProcess/IVolumeSource.h"

#include <cstdint>
#include <vector>

namespace PostProcess
{

using VolumeHandle = uint32_t;
constexpr VolumeHandle kInvalidVolumeHandle = 0xFFFFFFFFu;

class VolumeSystem : public IVolumeSource
{
public:
    VolumeSystem()           = default;
    ~VolumeSystem() override = default;

    VolumeSystem(const VolumeSystem&)            = delete;
    VolumeSystem& operator=(const VolumeSystem&) = delete;

    // Registers a copy of @p v, returns a stable handle valid until Unregister.
    VolumeHandle Register(const Volume& v);

    // Removes @p h. Safe to call with an invalid/already-removed handle.
    void Unregister(VolumeHandle h);

    // Mutable access — Editor/gameplay edits volumes in place via these.
    // Returns nullptr for invalid handles.
    Volume*       Get(VolumeHandle h);
    const Volume* Get(VolumeHandle h) const;

    // IVolumeSource — walks every occupied slot, computes weight for
    // ctx.cameraPos, appends snapshots with weight > 0 (unsorted — Stack
    // sorts globally after merging all sources).
    void Gather(const Context& ctx, std::vector<Snapshot>& out) override;

    // Iteration helpers for Editor UI.
    // Iterates all occupied handles in insertion order.
    template <typename F>
    void ForEach(F&& fn) const
    {
        for (size_t i = 0; i < m_slots.size(); ++i)
            if (m_slots[i].occupied)
                fn(static_cast<VolumeHandle>(i), m_slots[i].volume);
    }

    // Count of live volumes (for UI).
    size_t Size() const { return m_slots.size() - m_freeList.size(); }

private:
    struct Slot
    {
        bool   occupied = false;
        Volume volume;
    };

    std::vector<Slot>     m_slots;
    std::vector<uint32_t> m_freeList;
};

} // namespace PostProcess
