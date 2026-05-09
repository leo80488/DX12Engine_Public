#pragma once

// PostProcess::ParameterBlender — walks a priority-sorted list of volume
// snapshots and layers their overrides onto a base ParameterStore.
//
// Contract:
//   Blend(base, snapshots, out)
//       out starts as a copy of base, then each snapshot with a present
//       override for a given stage lerps that stage's block toward the
//       volume's values by the snapshot's weight.
//   Snapshots MUST be sorted by priority ascending — the caller (VolumeSystem
//   in practice) guarantees this.
//
// All per-stage blends operate on the whole block (not per-field). If a
// volume only wants to tweak a single field, populate the optional with a
// payload whose other fields match the base. The Editor UI creates overrides
// by first copying base values so this happens automatically.

#include "PostProcess/ParameterStore.h"
#include "PostProcess/Volume.h"   // Snapshot

#include <vector>

namespace PostProcess
{

class ParameterBlender
{
public:
    void Blend(const ParameterStore& base,
               const std::vector<Snapshot>& snapshots,
               ParameterStore& out) const;
};

} // namespace PostProcess
