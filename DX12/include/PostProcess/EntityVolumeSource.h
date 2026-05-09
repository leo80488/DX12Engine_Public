#pragma once

// PostProcess::EntityVolumeSource — IVolumeSource implementation backed by
// the ECS. Walks every entity with an ECS::VolumeComponent + GlobalTransform,
// uses the transform's translation as the volume center (the component's
// stored center is ignored), and emits snapshots into the shared pool.
//
// Stateless beyond the interface contract — the component data itself
// lives inside the World's ComponentPool, so snapshots reference the live
// VolumeOverride pointer directly (valid until the next mutation).

#include "PostProcess/IVolumeSource.h"

namespace PostProcess
{

class EntityVolumeSource : public IVolumeSource
{
public:
    EntityVolumeSource()           = default;
    ~EntityVolumeSource() override = default;

    void Gather(const Context& ctx, std::vector<Snapshot>& out) override;
};

} // namespace PostProcess
