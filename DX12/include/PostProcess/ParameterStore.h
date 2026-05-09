#pragma once

// ParameterStore — authoritative, in-memory container for every Stage's
// parameter block. Lives on PostProcess::Stack (one instance per stack);
// passed to effect adapters via Context::params each frame.
//
// Phase 2 (current): mutated by EditorLayer UI and PostProcessConfig; read
// by adapter Execute().
// Phase 3 (planned): VolumeBlender writes into the store between UI writes
// and adapter reads, overlaying volume-driven overrides on the base values.

#include "PostProcess/PostProcessParams.h"

namespace PostProcess
{

class ParameterStore
{
public:
    ParameterStore() = default;

    // Mutable accessors — EditorLayer / config load / future blender write here.
    CASParams&          GetCAS()          { return m_cas; }
    AutoExposureParams& GetAutoExposure() { return m_autoExposure; }
    BloomParams&        GetBloom()        { return m_bloom; }
    TonemappingParams&  GetTonemapping()  { return m_tonemapping; }

    // Const accessors — adapter Execute() + config save read here.
    const CASParams&          GetCAS()          const { return m_cas; }
    const AutoExposureParams& GetAutoExposure() const { return m_autoExposure; }
    const BloomParams&        GetBloom()        const { return m_bloom; }
    const TonemappingParams&  GetTonemapping()  const { return m_tonemapping; }

private:
    CASParams          m_cas;
    AutoExposureParams m_autoExposure;
    BloomParams        m_bloom;
    TonemappingParams  m_tonemapping;
};

} // namespace PostProcess
