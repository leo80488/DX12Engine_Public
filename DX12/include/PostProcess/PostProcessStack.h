#pragma once

// PostProcess::Stack — fixed-slot container that executes IEffect instances
// in Stage order. Replaces the hardcoded CAS→AutoExposure→Bloom→ToneMap
// sequence in Renderer Phase 4.7.
//
// Phase 1 scope:
//   - Slot-indexed storage (one effect per Stage)
//   - Ordered Execute() with per-effect IsEnabled() gate (zero-cost skip)
//   - No RT pool, no ping-pong textures, no parameter store — effects still
//     manage their own inputs/outputs via the wrapped legacy pass.

#include "PostProcess/IPostProcessEffect.h"
#include "PostProcess/ParameterStore.h"
#include "PostProcess/ParameterBlender.h"
#include "PostProcess/ScriptedOverride.h"

#include <array>
#include <memory>
#include <vector>

namespace PostProcess
{

class IVolumeSource;

class Stack
{
public:
    Stack() = default;
    ~Stack() = default;

    Stack(const Stack&)            = delete;
    Stack& operator=(const Stack&) = delete;

    // Registers `effect` at its GetStage() slot. Overwrites any previously
    // registered effect for the same slot (last-writer-wins — fine for our
    // fixed 1:1 stage→adapter mapping in Phase 1).
    void RegisterEffect(std::unique_ptr<IEffect> effect);

    // Walks stages in enum order, calling Execute() on each enabled effect.
    // `ctx.cl` must be a valid compute command list recorded serially on a
    // single thread. The stack writes its owned ParameterStore into
    // ctx.params before dispatch — callers don't need to set it themselves.
    void Execute(Context& ctx);

    // Direct slot query — useful for EditorLayer debug UI / introspection.
    IEffect* GetEffect(Stage s) const
    {
        return m_effects[static_cast<size_t>(s)].get();
    }

    // Base parameter container — EditorLayer UI and .ippc load/save touch
    // this. Adapters see a **blended** copy via Context::params, produced
    // from base + volume snapshots each Execute().
    ParameterStore&       GetParameters()       { return m_baseParams; }
    const ParameterStore& GetParameters() const { return m_baseParams; }

    // Register a volume source. Multiple sources coexist — typical setup:
    //   - VolumeSystem        (slot registry, editor/scripted non-entity use)
    //   - EntityVolumeSource  (ECS VolumeComponent)
    // Renderer adds both in Compile(). Non-owning pointers; caller must keep
    // the sources alive while they're registered.
    void AddVolumeSource(IVolumeSource* src);
    void RemoveVolumeSource(IVolumeSource* src);

    // Transient, time-driven overrides (damage flash, flashbang, etc).
    // Lives on Stack because its lifecycle matches the post-process chain.
    ScriptedOverrideSystem&       GetScriptedOverrides()       { return m_scriptedOverrides; }
    const ScriptedOverrideSystem& GetScriptedOverrides() const { return m_scriptedOverrides; }

private:
    std::array<std::unique_ptr<IEffect>,
               static_cast<size_t>(Stage::Count)> m_effects{};

    ParameterStore              m_baseParams;       // UI / config / script writes here
    ParameterStore              m_blendedParams;    // blend(base, all sources) — adapters read
    ParameterBlender            m_blender;
    std::vector<IVolumeSource*> m_volumeSources;
    ScriptedOverrideSystem      m_scriptedOverrides;
};

} // namespace PostProcess
